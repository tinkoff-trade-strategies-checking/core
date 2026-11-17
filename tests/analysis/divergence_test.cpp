#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/analysis/divergence.hpp"
#include "run/run_test_support.hpp"

namespace {

using namespace core;
using core::testing::dec;
using core::testing::loadProgram;

// Одна бумага (лот 1, шаг 0.01), издержки нулевые, без лимита ликвидности.
// close по шагам 100, 100, 110, 110, 105, 105; open шага - close предыдущего,
// open шага 0 = 100.
struct Market {
  SessionSpec spec;
  std::shared_ptr<const Timeline> timeline;
  Market() {
    auto [s, t] = core::testing::syntheticTimeline(
        {"100", "100", "110", "110", "105", "105"});
    spec = s;
    spec.costs = CostConfig::zero();
    timeline = t;
  }
};

// Правило срабатывает только на шаге 1 (close 100 и предыдущий close 100) или
// только на шаге 3 (110 и 110).
std::string atStep1(const std::string &action) {
  return "rule \"r\" when close == 100 and close[1] == 100 then " + action +
         " because \"сигнал на шаге 1\"\n";
}
std::string atStep3(const std::string &action) {
  return "rule \"r\" when close == 110 and close[1] == 110 then " + action +
         " because \"сигнал на шаге 3\"\n";
}
const std::string kSilent =
    "rule \"r\" when false then buy 1 lots because \"никогда\"\n";

AlgorithmInput algo(const std::string &id, const std::string &source) {
  auto loaded = loadProgram(source);
  return AlgorithmInput{
      id, std::move(loaded.program), std::move(loaded.params), {}};
}

// Program не копируется, поэтому список алгоритмов собирается так, а не
// фигурными скобками.
template <typename... Inputs>
std::vector<AlgorithmInput> algos(Inputs &&...inputs) {
  std::vector<AlgorithmInput> list;
  list.reserve(sizeof...(Inputs));
  (list.push_back(std::move(inputs)), ...);
  return list;
}

JournalEntry place(std::int64_t seq, std::int64_t step, Side side,
                   OrderType type, std::int64_t lots,
                   std::optional<Decimal> stop = std::nullopt,
                   std::optional<Decimal> limit = std::nullopt) {
  OrderSpec spec;
  spec.side = side;
  spec.type = type;
  spec.lots = lots;
  spec.stopPrice = stop;
  spec.limitPrice = limit;
  JournalEntry e;
  e.seq = seq;
  e.step = step;
  e.command = PlaceCmd{spec, std::nullopt};
  return e;
}

JournalEntry market(std::int64_t seq, std::int64_t step, Side side,
                    std::int64_t lots) {
  return place(seq, step, side, OrderType::Market, lots);
}

DivergenceResult analyze(const Market &m,
                         const std::vector<JournalEntry> &journal,
                         const std::vector<AlgorithmInput> &algorithms) {
  return analyzeDivergences(m.spec, m.timeline, journal,
                            m.timeline->totalSteps() - 1, algorithms);
}

} // namespace

// Алгоритм покупает 10 лотов на шаге 1, человек бездействует: горизонт -
// последний шаг, исполнение по open шага 2.
TEST(Divergence, Missed) {
  Market m;
  const DivergenceResult r =
      analyze(m, {}, algos(algo("ref", atStep1("buy 10 lots"))));

  ASSERT_EQ(r.divergences.size(), 1u);
  const Divergence &d = r.divergences[0];
  EXPECT_EQ(d.algorithmId, "ref");
  EXPECT_EQ(d.type, DivergenceType::Missed);
  EXPECT_EQ(d.humanStep, -1);
  EXPECT_EQ(d.algoStep, 1);
  EXPECT_EQ(d.humanLots, 0);
  EXPECT_EQ(d.algoLots, 10);
  EXPECT_EQ(d.horizonStep, 5);
  EXPECT_EQ(d.cost.toString(2), "50.00");
  EXPECT_NEAR(d.costPercent, 50.0 / 1000000.0 * 100.0, 1e-9);
  EXPECT_EQ(d.reason, "сигнал на шаге 1");
  EXPECT_TRUE(r.missed.empty());

  // Независимый портфель алгоритма - обычный прогон: 1 000 000 - 1000 + 10 *
  // 105.
  ASSERT_EQ(r.independent.size(), 1u);
  EXPECT_EQ(r.independent[0].finalEquity.toString(2), "1000050.00");
}

// Человек покупает на шаге 1 и продаёт на шаге 3, алгоритм молчит.
TEST(Divergence, ExcessWithProfit) {
  Market m;
  const std::vector<JournalEntry> journal{market(0, 1, Side::Buy, 10),
                                          market(1, 3, Side::Sell, 10)};
  const DivergenceResult r = analyze(m, journal, algos(algo("ref", kSilent)));

  ASSERT_EQ(r.divergences.size(), 2u);
  const Divergence &buy = r.divergences[0];
  EXPECT_EQ(buy.type, DivergenceType::Excess);
  EXPECT_EQ(buy.humanStep, 1);
  EXPECT_EQ(buy.algoStep, -1);
  EXPECT_EQ(buy.humanLots, 10);
  EXPECT_EQ(buy.algoLots, 0);
  EXPECT_EQ(buy.horizonStep, 3);
  EXPECT_EQ(buy.cost.toString(2), "-100.00");
  EXPECT_EQ(buy.reason, "алгоритм здесь не действовал бы");

  const Divergence &sell = r.divergences[1];
  EXPECT_EQ(sell.type, DivergenceType::Excess);
  EXPECT_EQ(sell.humanStep, 3);
  EXPECT_EQ(sell.humanLots, -10);
  EXPECT_EQ(sell.horizonStep, 5);
  EXPECT_EQ(sell.cost.toString(2), "-50.00");

  // ФТ-20: сделка человека 2 -> 4 с прибылью 100 при нулевых издержках, вход -
  // «лишнее действие».
  ASSERT_EQ(r.missed.size(), 1u);
  EXPECT_EQ(r.missed[0].instrument, 0);
  EXPECT_EQ(r.missed[0].entryStep, 2);
  EXPECT_EQ(r.missed[0].exitStep, 4);
  EXPECT_EQ(r.missed[0].lots, 10);
  EXPECT_EQ(r.missed[0].netPnl.toString(2), "100.00");
  EXPECT_EQ(r.missed[0].algorithmIds, std::vector<std::string>{"ref"});
  // Цены пары за штуку: вход по open шага 2 (= close шага 1), выход по open
  // шага 4 (= close шага 3).
  EXPECT_EQ(r.missed[0].entryPrice, dec("100"));
  EXPECT_EQ(r.missed[0].exitPrice, dec("110"));
}

TEST(Divergence, TimingWithinWindow) {
  Market m;
  const DivergenceResult r =
      analyze(m, {market(0, 1, Side::Buy, 10)},
              algos(algo("ref", atStep3("buy 10 lots"))));

  ASSERT_EQ(r.divergences.size(), 1u);
  const Divergence &d = r.divergences[0];
  EXPECT_EQ(d.type, DivergenceType::Timing);
  EXPECT_EQ(d.humanStep, 1);
  EXPECT_EQ(d.algoStep, 3);
  EXPECT_EQ(d.humanLots, 10);
  EXPECT_EQ(d.algoLots, 10);
  EXPECT_EQ(d.horizonStep, 5);
  // Человек: 10 * (105 - 100) = 50; алгоритм купил бы по open шага 4 = 110: 10
  // * (105 - 110) = -50.
  EXPECT_EQ(d.cost.toString(2), "-100.00");
  EXPECT_EQ(d.reason, "сигнал на шаге 3");
  EXPECT_TRUE(r.missed.empty());
}

// Окно W = 3: расстояние 4 шага - уже не «момент», а «лишнее» и «упущенное».
TEST(Divergence, TimingOutsideWindow) {
  Market m;
  const DivergenceResult r =
      analyze(m, {market(0, 0, Side::Buy, 10)},
              algos(algo("ref", atStep1("buy 10 lots"))));
  // Расстояние 1 - в окне.
  ASSERT_EQ(r.divergences.size(), 1u);
  EXPECT_EQ(r.divergences[0].type, DivergenceType::Timing);

  auto far =
      analyze(m, {market(0, 0, Side::Buy, 10)},
              algos(algo("ref", "rule \"r\" when close == 105 and close[1] == "
                                "110 then buy 10 lots because \"поздно\"\n")));
  // Сигнал на шаге 4 (на последнем шаге сессия уже закончена, заявок нет),
  // человек на шаге 0: |Δt| = 4 > 3.
  ASSERT_EQ(far.divergences.size(), 2u);
  EXPECT_EQ(far.divergences[0].type, DivergenceType::Excess);
  EXPECT_EQ(far.divergences[1].type, DivergenceType::Missed);
}

TEST(Divergence, SizeOver20Percent) {
  Market m;
  const DivergenceResult over =
      analyze(m, {market(0, 1, Side::Buy, 10)},
              algos(algo("ref", atStep1("buy 13 lots"))));
  ASSERT_EQ(over.divergences.size(), 1u);
  const Divergence &d = over.divergences[0];
  EXPECT_EQ(d.type, DivergenceType::Size);
  EXPECT_EQ(d.humanStep, 1);
  EXPECT_EQ(d.algoStep, 1);
  EXPECT_EQ(d.humanLots, 10);
  EXPECT_EQ(d.algoLots, 13);
  EXPECT_EQ(d.horizonStep, 5);
  EXPECT_EQ(d.cost.toString(2), "15.00"); // 13 * 5 - 10 * 5

  const DivergenceResult under =
      analyze(m, {market(0, 1, Side::Buy, 10)},
              algos(algo("ref", atStep1("buy 11 lots"))));
  EXPECT_TRUE(under.divergences.empty());
  EXPECT_TRUE(under.consensus.empty());
}

TEST(Divergence, OppositeSameStep) {
  Market m;
  const DivergenceResult r =
      analyze(m, {market(0, 1, Side::Buy, 10)},
              algos(algo("ref", atStep1("sell 10 lots"))));

  ASSERT_EQ(r.divergences.size(), 1u);
  const Divergence &d = r.divergences[0];
  EXPECT_EQ(d.type, DivergenceType::Excess);
  EXPECT_EQ(d.humanStep, 1);
  EXPECT_EQ(d.algoStep, 1);
  EXPECT_EQ(d.humanLots, 10);
  EXPECT_EQ(d.algoLots, -10);
  EXPECT_EQ(d.reason, "алгоритм здесь не действовал бы");
  // Продажа алгоритма при нулевой позиции человека отклоняется движком: V
  // алгоритма 0, V человека 50.
  EXPECT_EQ(d.cost.toString(2), "-50.00");
  EXPECT_TRUE(std::none_of(
      r.divergences.begin(), r.divergences.end(),
      [](const Divergence &x) { return x.type == DivergenceType::Missed; }));
}

TEST(Divergence, Consensus) {
  Market m;
  const std::vector<AlgorithmInput> algorithms =
      algos(algo("a", atStep1("buy 10 lots")),
            algo("b", atStep1("buy 10 lots")), algo("c", kSilent));
  const DivergenceResult r =
      analyze(m, {market(0, 1, Side::Buy, 10)}, algorithms);

  ASSERT_EQ(r.divergences.size(), 1u);
  EXPECT_EQ(r.divergences[0].algorithmId, "c");
  EXPECT_EQ(r.divergences[0].type, DivergenceType::Excess);

  ASSERT_EQ(r.consensus.size(), 1u);
  const ConsensusItem &item = r.consensus[0];
  EXPECT_TRUE(item.humanAction);
  EXPECT_EQ(item.instrument, 0);
  EXPECT_EQ(item.step, 1);
  EXPECT_EQ(item.side, Side::Buy);
  EXPECT_EQ(item.agreeing, 2);
  EXPECT_EQ(item.total, 3);
  EXPECT_EQ(item.medianCost.toString(2), "-50.00");
  EXPECT_EQ(r.independent.size(), 3u);
  // Человек купил, и никто из упомянутых не пропустил его вход: сделок для
  // ФТ-20 нет (сделка не закрыта).
  EXPECT_TRUE(r.missed.empty());
}

// Упущенные действия группируются по (бумага, шаг, сторона); медиана при чётном
// числе - среднее двух средних.
TEST(Divergence, ConsensusOfMissedActions) {
  Market m;
  const std::vector<AlgorithmInput> algorithms =
      algos(algo("a", atStep1("buy 10 lots")),
            algo("b", atStep1("buy 20 lots")), algo("c", kSilent));
  const DivergenceResult r = analyze(m, {}, algorithms);

  ASSERT_EQ(r.divergences.size(), 2u);
  ASSERT_EQ(r.consensus.size(), 1u);
  const ConsensusItem &item = r.consensus[0];
  EXPECT_FALSE(item.humanAction);
  EXPECT_EQ(item.step, 1);
  EXPECT_EQ(item.side, Side::Buy);
  EXPECT_EQ(item.agreeing, 2);
  EXPECT_EQ(item.total, 3);
  // Цены ошибок 50 и 100: среднее 75.
  EXPECT_EQ(item.medianCost.toString(2), "75.00");
}

// Стоп человека не создаёт расхождения (защитные заявки не участвуют ни у
// человека, ни у алгоритма).
TEST(Divergence, ProtectiveOrdersIgnored) {
  Market m;
  const DivergenceResult onlyStop =
      analyze(m, {place(0, 1, Side::Buy, OrderType::Stop, 10, dec("120"))},
              algos(algo("ref", kSilent)));
  EXPECT_TRUE(onlyStop.divergences.empty());
  EXPECT_TRUE(onlyStop.consensus.empty());

  // Покупка и стоп на продажу: расхождение - только у покупки.
  const DivergenceResult withStop =
      analyze(m,
              {market(0, 1, Side::Buy, 10),
               place(1, 2, Side::Sell, OrderType::Stop, 10, dec("90"))},
              algos(algo("ref", kSilent)));
  ASSERT_EQ(withStop.divergences.size(), 1u);
  EXPECT_EQ(withStop.divergences[0].humanStep, 1);

  // Заявка человека с OCO (даже если главная нога лимитная) - защитная связка,
  // не действие.
  OrderSpec limitSell;
  limitSell.side = Side::Sell;
  limitSell.type = OrderType::Limit;
  limitSell.lots = 10;
  limitSell.limitPrice = dec("120");
  OrderSpec stopSell;
  stopSell.side = Side::Sell;
  stopSell.type = OrderType::Stop;
  stopSell.lots = 10;
  stopSell.stopPrice = dec("90");
  JournalEntry oco;
  oco.seq = 1;
  oco.step = 2;
  oco.command = PlaceCmd{limitSell, stopSell};
  const std::vector<JournalEntry> withOco{market(0, 1, Side::Buy, 10), oco};
  ASSERT_TRUE(replaySession(m.spec, m.timeline, withOco, 5, 0)
                  .commandResults[1]
                  .accepted);
  const DivergenceResult ocoResult =
      analyze(m, withOco, algos(algo("ref", kSilent)));
  ASSERT_EQ(ocoResult.divergences.size(), 1u);
  EXPECT_EQ(ocoResult.divergences[0].humanStep, 1);

  // Защитная заявка алгоритма тоже не действие.
  const DivergenceResult algoStop =
      analyze(m, {}, algos(algo("ref", atStep1("buy 10 lots stop 120"))));
  EXPECT_TRUE(algoStop.divergences.empty());
}

// Лимитная заявка - действие, как и рыночная.
TEST(Divergence, LimitOrderIsAnAction) {
  Market m;
  const DivergenceResult r = analyze(
      m,
      {place(0, 1, Side::Buy, OrderType::Limit, 10, std::nullopt, dec("99"))},
      algos(algo("ref", kSilent)));
  ASSERT_EQ(r.divergences.size(), 1u);
  EXPECT_EQ(r.divergences[0].type, DivergenceType::Excess);
  EXPECT_EQ(r.divergences[0].humanLots, 10);
  // Лимит 99 ниже рынка: не исполнилась, цена ошибки 0.
  EXPECT_EQ(r.divergences[0].cost.toString(2), "0.00");
}

// Алгоритм получает портфель человека: продажа возможна только потому, что
// человек уже купил.
TEST(Divergence, AlgorithmSeesHumanPosition) {
  Market m;
  const DivergenceResult r =
      analyze(m, {market(0, 1, Side::Buy, 10)},
              algos(algo("ref", "rule \"r\" when position > 0 and close == 110 "
                                "and close[1] == 110 then sell 10 lots "
                                "because \"выход\"\n")));

  ASSERT_EQ(r.divergences.size(), 2u);
  const Divergence &excess = r.divergences[0];
  EXPECT_EQ(excess.type, DivergenceType::Excess);
  EXPECT_EQ(excess.horizonStep,
            3); // следующее действие - продажа алгоритма на шаге 3
  EXPECT_EQ(excess.cost.toString(2), "-100.00");
  const Divergence &missed = r.divergences[1];
  EXPECT_EQ(missed.type, DivergenceType::Missed);
  EXPECT_EQ(missed.algoStep, 3);
  EXPECT_EQ(missed.algoLots, -10);
  // Алгоритм продаёт по open шага 4 = 110, close шага 5 = 105: -10 * (105 -
  // 110) = 50.
  EXPECT_EQ(missed.cost.toString(2), "50.00");
}

TEST(Divergence, Deterministic) {
  Market m;
  const std::vector<JournalEntry> journal{market(0, 1, Side::Buy, 10),
                                          market(1, 3, Side::Sell, 10)};
  const std::vector<AlgorithmInput> algorithms = algos(
      algo("a", atStep1("buy 13 lots")), algo("b", atStep3("sell 10 lots")));
  const DivergenceResult first = analyze(m, journal, algorithms);
  const DivergenceResult second = analyze(m, journal, algorithms);
  ASSERT_EQ(first.divergences.size(), second.divergences.size());
  for (std::size_t i = 0; i < first.divergences.size(); ++i) {
    EXPECT_EQ(first.divergences[i].type, second.divergences[i].type);
    EXPECT_EQ(first.divergences[i].cost, second.divergences[i].cost);
    EXPECT_EQ(first.divergences[i].algorithmId,
              second.divergences[i].algorithmId);
  }
  ASSERT_EQ(first.independent.size(), second.independent.size());
  for (std::size_t i = 0; i < first.independent.size(); ++i) {
    EXPECT_EQ(first.independent[i].digest, second.independent[i].digest);
  }
}

// Журнал человека не меняется от разбора, а сам разбор не зависит от числа
// алгоритмов у соседей (каждый отдельно).
TEST(Divergence, AlgorithmsAreIndependent) {
  Market m;
  const std::vector<JournalEntry> journal{market(0, 1, Side::Buy, 10)};
  const DivergenceResult alone =
      analyze(m, journal, algos(algo("a", atStep1("buy 13 lots"))));
  const DivergenceResult together =
      analyze(m, journal,
              algos(algo("b", atStep3("buy 10 lots")),
                    algo("a", atStep1("buy 13 lots"))));
  ASSERT_EQ(alone.divergences.size(), 1u);
  ASSERT_EQ(together.divergences.size(), 2u);
  EXPECT_EQ(together.divergences[0].algorithmId, "b");
  EXPECT_EQ(together.divergences[1].algorithmId, "a");
  EXPECT_EQ(together.divergences[1].cost, alone.divergences[0].cost);
}

TEST(Divergence, InvalidInput) {
  Market m;
  const std::vector<AlgorithmInput> one = algos(algo("a", kSilent));
  // lastStep вне шкалы.
  EXPECT_THROW(analyzeDivergences(m.spec, m.timeline, {}, 6, one),
               std::invalid_argument);
  EXPECT_THROW(analyzeDivergences(m.spec, m.timeline, {}, -1, one),
               std::invalid_argument);
  // naive.
  SessionSpec naive = m.spec;
  naive.naive = true;
  EXPECT_THROW(analyzeDivergences(naive, m.timeline, {}, 5, one),
               std::invalid_argument);
  // Больше пяти алгоритмов и повтор id.
  std::vector<AlgorithmInput> six;
  for (int i = 0; i < 6; ++i)
    six.push_back(algo("a" + std::to_string(i), kSilent));
  EXPECT_THROW(analyze(m, {}, six), std::invalid_argument);
  EXPECT_THROW(analyze(m, {}, algos(algo("a", kSilent), algo("a", kSilent))),
               std::invalid_argument);
  // Журнал за lastStep.
  EXPECT_THROW(
      analyzeDivergences(m.spec, m.timeline,
                         std::vector<JournalEntry>{market(0, 6, Side::Buy, 1)},
                         5, one),
      std::invalid_argument);
}

// Без алгоритмов сравнивать не с чем.
TEST(Divergence, NoAlgorithms) {
  Market m;
  const DivergenceResult r = analyze(m, {market(0, 1, Side::Buy, 10)}, {});
  EXPECT_TRUE(r.divergences.empty());
  EXPECT_TRUE(r.consensus.empty());
  EXPECT_TRUE(r.independent.empty());
}

// INV-1: действия алгоритма на шагах до t не зависят от баров после t (меняем
// только хвост шкалы).
TEST(Divergence, AlgorithmActionsDoNotDependOnFutureBars) {
  Market base;
  Market changed;
  auto [spec, timeline] = core::testing::syntheticTimeline(
      {"100", "100", "110", "110", "50", "300"});
  changed.spec = spec;
  changed.spec.costs = CostConfig::zero();
  changed.timeline = timeline;
  const std::string rule =
      "rule \"r\" when sma(close, 2) > 104 and close[1] == 100 then buy 10 "
      "lots because \"{sma(close, 2)}\"\n";

  const DivergenceResult a = analyze(base, {}, algos(algo("ref", rule)));
  const DivergenceResult b = analyze(changed, {}, algos(algo("ref", rule)));
  ASSERT_EQ(a.divergences.size(), 1u);
  ASSERT_EQ(b.divergences.size(), 1u);
  EXPECT_EQ(a.divergences[0].algoStep, 2);
  EXPECT_EQ(a.divergences[0].algoStep, b.divergences[0].algoStep);
  EXPECT_EQ(a.divergences[0].algoLots, b.divergences[0].algoLots);
  EXPECT_EQ(a.divergences[0].reason, b.divergences[0].reason);
  // Цена ошибки смотрит вперёд до горизонта (это разбор постфактум) и потому
  // различается.
  EXPECT_NE(a.divergences[0].cost, b.divergences[0].cost);
}

// Сессия закончилась раньше конца шкалы: горизонты, V и независимые портфели -
// только по шагам до lastStep.
TEST(Divergence, EarlyFinishUsesOnlyStepsUpToLastStep) {
  Market m;
  Market changed; // тот же отрезок 0…3, но другой хвост
  auto [spec, timeline] = core::testing::syntheticTimeline(
      {"100", "100", "110", "110", "50", "300"});
  changed.spec = spec;
  changed.spec.costs = CostConfig::zero();
  changed.timeline = timeline;
  const std::int64_t last = 3;

  const std::vector<JournalEntry> journal{market(0, 1, Side::Buy, 10)};
  const auto run = [&](const Market &market) {
    return analyzeDivergences(
        market.spec, market.timeline, journal, last,
        algos(algo("ref", atStep1("buy 13 lots")), algo("silent", kSilent)));
  };
  const DivergenceResult a = run(m);
  const DivergenceResult b = run(changed);

  ASSERT_EQ(a.divergences.size(), 2u);
  // Размер: горизонт - lastStep, обе покупки по open шага 2 = 100, close(3) =
  // 110: 13 * 10 - 10 * 10 = 30.
  EXPECT_EQ(a.divergences[0].algorithmId, "ref");
  EXPECT_EQ(a.divergences[0].type, DivergenceType::Size);
  EXPECT_EQ(a.divergences[0].horizonStep, last);
  EXPECT_EQ(a.divergences[0].cost.toString(2), "30.00");
  // Лишнее действие: V человека = 10 * (110 - 100) = 100 на close(lastStep).
  EXPECT_EQ(a.divergences[1].algorithmId, "silent");
  EXPECT_EQ(a.divergences[1].horizonStep, last);
  EXPECT_EQ(a.divergences[1].cost.toString(2), "-100.00");

  // Независимый портфель - прогон по тому же отрезку: 1 000 000 - 1300 + 13 *
  // 110 (покупка на шаге 1 исполнена на шаге 2).
  ASSERT_EQ(a.independent.size(), 2u);
  EXPECT_EQ(a.independent[0].lastStep, last);
  EXPECT_EQ(a.independent[0].equity.size(), static_cast<std::size_t>(last + 1));
  EXPECT_EQ(a.independent[0].finalEquity.toString(2), "1000130.00");
  EXPECT_EQ(a.independent[1].finalEquity.toString(2), "1000000.00");

  // Хвост шкалы за lastStep ни на что не влияет.
  ASSERT_EQ(b.divergences.size(), a.divergences.size());
  for (std::size_t i = 0; i < a.divergences.size(); ++i) {
    EXPECT_EQ(a.divergences[i].cost, b.divergences[i].cost);
    EXPECT_EQ(a.divergences[i].horizonStep, b.divergences[i].horizonStep);
    EXPECT_EQ(a.divergences[i].costPercent, b.divergences[i].costPercent);
  }
  for (std::size_t i = 0; i < a.independent.size(); ++i) {
    EXPECT_EQ(a.independent[i].digest, b.independent[i].digest);
  }
}

// Лимитная заявка, которая держится 50 баров, - одно действие, а не пятьдесят.
TEST(Divergence, RepeatedLimitIntentIsOneAction) {
  const std::vector<std::string> closes(50, "100");
  auto [spec, timeline] = core::testing::syntheticTimeline(closes);
  Market m;
  m.spec = spec;
  m.spec.costs = CostConfig::zero();
  m.timeline = timeline;
  const DivergenceResult r =
      analyze(m, {},
              algos(algo("ref", "rule \"r\" when true then buy 10 lots limit "
                                "90 because \"ждать 90\"\n")));
  ASSERT_EQ(r.divergences.size(), 1u);
  EXPECT_EQ(r.divergences[0].type, DivergenceType::Missed);
  EXPECT_EQ(r.divergences[0].algoStep, 0);

  // Рыночные заявки исполняются на следующем баре: повтор - новое действие
  // каждый раз (последний шаг - конец сессии).
  const DivergenceResult market = analyze(
      m, {},
      algos(
          algo("ref", "rule \"r\" when true then buy 1 lots because \"x\"\n")));
  EXPECT_EQ(market.divergences.size(), 49u);
}

// Подача и замена на одном шаге - одно действие, без ложного расхождения
// «размер».
TEST(Divergence, PlaceThenReplaceOnSameStepIsOneAction) {
  Market m;
  JournalEntry replace;
  replace.seq = 1;
  replace.step = 1;
  OrderSpec replacement;
  replacement.side = Side::Buy;
  replacement.type = OrderType::Market;
  replacement.lots = 10;
  replace.command = ReplaceCmd{0, PlaceCmd{replacement, std::nullopt}};
  const std::vector<JournalEntry> journal{market(0, 1, Side::Buy, 10), replace};
  ASSERT_TRUE(replaySession(m.spec, m.timeline, journal, 5, 0)
                  .commandResults[1]
                  .accepted);

  const DivergenceResult r =
      analyze(m, journal, algos(algo("ref", atStep1("buy 10 lots"))));
  EXPECT_TRUE(r.divergences.empty());

  // Замена заявки, поданной на прошлом шаге, - новое действие; прежнее остаётся
  // и сопоставляется само по себе.
  replace.step = 2;
  const std::vector<JournalEntry> later{
      place(0, 1, Side::Buy, OrderType::Limit, 10, std::nullopt, dec("90")),
      replace};
  ASSERT_TRUE(replaySession(m.spec, m.timeline, later, 5, 0)
                  .commandResults[1]
                  .accepted);
  const DivergenceResult r2 =
      analyze(m, later, algos(algo("ref", atStep1("buy 10 lots"))));
  ASSERT_EQ(r2.divergences.size(), 1u);
  EXPECT_EQ(r2.divergences[0].humanStep, 2);
  EXPECT_EQ(r2.divergences[0].type, DivergenceType::Excess);
}

// Медиана консенсуса - только по алгоритмам, которые «здесь не действовали бы»;
// «размер» и «момент» в неё не входят.
TEST(Divergence, ConsensusMedianOnlyOverNonAgreeingAlgorithms) {
  Market m;
  const DivergenceResult r =
      analyze(m, {market(0, 1, Side::Buy, 10)},
              algos(algo("a", atStep1("buy 13 lots")),
                    algo("b", atStep1("buy 13 lots")), algo("c", kSilent)));
  ASSERT_EQ(r.divergences.size(),
            3u); // два «размера» по 15.00 и одно «лишнее» -50.00
  ASSERT_EQ(r.consensus.size(), 1u);
  EXPECT_EQ(r.consensus[0].agreeing, 2);
  EXPECT_EQ(r.consensus[0].total, 3);
  EXPECT_EQ(r.consensus[0].medianCost.toString(2), "-50.00");
}

// Прогрев: алгоритм на месте человека видит историю до шага 0 (как и в
// независимом прогоне).
TEST(Divergence, WarmupHistoryIsVisibleToAlgorithm) {
  Market m;
  // История: 90 и 95 до шага 0. Правило срабатывает только на шаге 0 и только с
  // историей: close 100, close[1] 95.
  constexpr std::int64_t kDayMs = 86'400'000;
  constexpr std::int64_t kStart =
      1'704'747'600'000; // 2024-01-09 00:00 МСК - шаг 0 рынка
  std::vector<Bar> history;
  for (int i = 0; i < 2; ++i) {
    Bar b;
    b.tsOpenMs = kStart - (2 - i) * kDayMs;
    b.availableAtMs = b.tsOpenMs + kDayMs;
    b.open = b.close = dec(i == 0 ? "90" : "95");
    b.high = b.close + dec("0.5");
    b.low = b.close - dec("0.5");
    b.volume = 1'000'000;
    history.push_back(b);
  }
  const Timeline before =
      Timeline::build({core::testing::syntheticInstrument()}, {history}, {});
  const std::string rule = "rule \"r\" when close == 100 and close[1] == 95 "
                           "then buy 10 lots because \"после 95\"\n";

  const DivergenceResult plain = analyze(m, {}, algos(algo("ref", rule)));
  EXPECT_TRUE(plain.divergences.empty());

  m.timeline =
      std::make_shared<const Timeline>(m.timeline->withWarmup(before, 2));
  const DivergenceResult warm = analyze(m, {}, algos(algo("ref", rule)));
  ASSERT_EQ(warm.divergences.size(), 1u);
  EXPECT_EQ(warm.divergences[0].type, DivergenceType::Missed);
  EXPECT_EQ(warm.divergences[0].algoStep, 0);
  EXPECT_EQ(warm.divergences[0].reason, "после 95");
}
