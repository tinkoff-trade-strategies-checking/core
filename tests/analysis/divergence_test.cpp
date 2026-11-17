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
  SessionSpec Spec;
  std::shared_ptr<const Timeline> TimelineRef;
  Market() {
    auto [s, t] = core::testing::syntheticTimeline(
        {"100", "100", "110", "110", "105", "105"});
    Spec = s;
    Spec.Costs = CostConfig::zero();
    TimelineRef = t;
  }
};

// Правило срабатывает только на шаге 1 (close 100 и предыдущий close 100) или
// только на шаге 3 (110 и 110).
std::string atStep1(const std::string &Action) {
  return "rule \"r\" when close == 100 and close[1] == 100 then " + Action +
         " because \"сигнал на шаге 1\"\n";
}
std::string atStep3(const std::string &Action) {
  return "rule \"r\" when close == 110 and close[1] == 110 then " + Action +
         " because \"сигнал на шаге 3\"\n";
}
const std::string kSilent =
    "rule \"r\" when false then buy 1 lots because \"никогда\"\n";

AlgorithmInput algo(const std::string &Id, const std::string &Source) {
  auto Loaded = loadProgram(Source);
  return AlgorithmInput{
      Id, std::move(Loaded.Program), std::move(Loaded.Params), {}};
}

// Program не копируется, поэтому список алгоритмов собирается так, а не
// фигурными скобками.
template <typename... Args>
std::vector<AlgorithmInput> algos(Args &&...Inputs) {
  std::vector<AlgorithmInput> List;
  List.reserve(sizeof...(Inputs));
  (List.push_back(std::move(Inputs)), ...);
  return List;
}

JournalEntry place(std::int64_t Seq, std::int64_t Step, Side Side,
                   OrderType Type, std::int64_t Lots,
                   std::optional<Decimal> Stop = std::nullopt,
                   std::optional<Decimal> Limit = std::nullopt) {
  OrderSpec Spec;
  Spec.OrderSide = Side;
  Spec.Type = Type;
  Spec.Lots = Lots;
  Spec.StopPrice = Stop;
  Spec.LimitPrice = Limit;
  JournalEntry E;
  E.Seq = Seq;
  E.Step = Step;
  E.Command = PlaceCmd{Spec, std::nullopt};
  return E;
}

JournalEntry market(std::int64_t Seq, std::int64_t Step, Side Side,
                    std::int64_t Lots) {
  return place(Seq, Step, Side, OrderType::Market, Lots);
}

DivergenceResult analyze(const Market &M,
                         const std::vector<JournalEntry> &Journal,
                         const std::vector<AlgorithmInput> &Algorithms) {
  return analyzeDivergences(M.Spec, M.TimelineRef, Journal,
                            M.TimelineRef->totalSteps() - 1, Algorithms);
}

} // namespace

// Алгоритм покупает 10 лотов на шаге 1, человек бездействует: горизонт -
// последний шаг, исполнение по open шага 2.
TEST(Divergence, Missed) {
  Market M;
  const DivergenceResult R =
      analyze(M, {}, algos(algo("ref", atStep1("buy 10 lots"))));

  ASSERT_EQ(R.Divergences.size(), 1u);
  const Divergence &D = R.Divergences[0];
  EXPECT_EQ(D.AlgorithmId, "ref");
  EXPECT_EQ(D.Type, DivergenceType::Missed);
  EXPECT_EQ(D.HumanStep, -1);
  EXPECT_EQ(D.AlgoStep, 1);
  EXPECT_EQ(D.HumanLots, 0);
  EXPECT_EQ(D.AlgoLots, 10);
  EXPECT_EQ(D.HorizonStep, 5);
  EXPECT_EQ(D.Cost.toString(2), "50.00");
  EXPECT_NEAR(D.CostPercent, 50.0 / 1000000.0 * 100.0, 1e-9);
  EXPECT_EQ(D.ReasonCode, "сигнал на шаге 1");
  EXPECT_TRUE(R.Missed.empty());

  // Независимый портфель алгоритма - обычный прогон: 1 000 000 - 1000 + 10 *
  // 105.
  ASSERT_EQ(R.Independent.size(), 1u);
  EXPECT_EQ(R.Independent[0].FinalEquity.toString(2), "1000050.00");
}

// Человек покупает на шаге 1 и продаёт на шаге 3, алгоритм молчит.
TEST(Divergence, ExcessWithProfit) {
  Market M;
  const std::vector<JournalEntry> Journal{market(0, 1, Side::Buy, 10),
                                          market(1, 3, Side::Sell, 10)};
  const DivergenceResult R = analyze(M, Journal, algos(algo("ref", kSilent)));

  ASSERT_EQ(R.Divergences.size(), 2u);
  const Divergence &Buy = R.Divergences[0];
  EXPECT_EQ(Buy.Type, DivergenceType::Excess);
  EXPECT_EQ(Buy.HumanStep, 1);
  EXPECT_EQ(Buy.AlgoStep, -1);
  EXPECT_EQ(Buy.HumanLots, 10);
  EXPECT_EQ(Buy.AlgoLots, 0);
  EXPECT_EQ(Buy.HorizonStep, 3);
  EXPECT_EQ(Buy.Cost.toString(2), "-100.00");
  EXPECT_EQ(Buy.ReasonCode, "алгоритм здесь не действовал бы");

  const Divergence &Sell = R.Divergences[1];
  EXPECT_EQ(Sell.Type, DivergenceType::Excess);
  EXPECT_EQ(Sell.HumanStep, 3);
  EXPECT_EQ(Sell.HumanLots, -10);
  EXPECT_EQ(Sell.HorizonStep, 5);
  EXPECT_EQ(Sell.Cost.toString(2), "-50.00");

  // ФТ-20: сделка человека 2 -> 4 с прибылью 100 при нулевых издержках, вход -
  // «лишнее действие».
  ASSERT_EQ(R.Missed.size(), 1u);
  EXPECT_EQ(R.Missed[0].Instrument, 0);
  EXPECT_EQ(R.Missed[0].EntryStep, 2);
  EXPECT_EQ(R.Missed[0].ExitStep, 4);
  EXPECT_EQ(R.Missed[0].Lots, 10);
  EXPECT_EQ(R.Missed[0].NetPnl.toString(2), "100.00");
  EXPECT_EQ(R.Missed[0].AlgorithmIds, std::vector<std::string>{"ref"});
  // Цены пары за штуку: вход по open шага 2 (= close шага 1), выход по open
  // шага 4 (= close шага 3).
  EXPECT_EQ(R.Missed[0].EntryPrice, dec("100"));
  EXPECT_EQ(R.Missed[0].ExitPrice, dec("110"));
}

TEST(Divergence, TimingWithinWindow) {
  Market M;
  const DivergenceResult R =
      analyze(M, {market(0, 1, Side::Buy, 10)},
              algos(algo("ref", atStep3("buy 10 lots"))));

  ASSERT_EQ(R.Divergences.size(), 1u);
  const Divergence &D = R.Divergences[0];
  EXPECT_EQ(D.Type, DivergenceType::Timing);
  EXPECT_EQ(D.HumanStep, 1);
  EXPECT_EQ(D.AlgoStep, 3);
  EXPECT_EQ(D.HumanLots, 10);
  EXPECT_EQ(D.AlgoLots, 10);
  EXPECT_EQ(D.HorizonStep, 5);
  // Человек: 10 * (105 - 100) = 50; алгоритм купил бы по open шага 4 = 110: 10
  // * (105 - 110) = -50.
  EXPECT_EQ(D.Cost.toString(2), "-100.00");
  EXPECT_EQ(D.ReasonCode, "сигнал на шаге 3");
  EXPECT_TRUE(R.Missed.empty());
}

// Окно W = 3: расстояние 4 шага - уже не «момент», а «лишнее» и «упущенное».
TEST(Divergence, TimingOutsideWindow) {
  Market M;
  const DivergenceResult R =
      analyze(M, {market(0, 0, Side::Buy, 10)},
              algos(algo("ref", atStep1("buy 10 lots"))));
  // Расстояние 1 - в окне.
  ASSERT_EQ(R.Divergences.size(), 1u);
  EXPECT_EQ(R.Divergences[0].Type, DivergenceType::Timing);

  auto Far =
      analyze(M, {market(0, 0, Side::Buy, 10)},
              algos(algo("ref", "rule \"r\" when close == 105 and close[1] == "
                                "110 then buy 10 lots because \"поздно\"\n")));
  // Сигнал на шаге 4 (на последнем шаге сессия уже закончена, заявок нет),
  // человек на шаге 0: |Δt| = 4 > 3.
  ASSERT_EQ(Far.Divergences.size(), 2u);
  EXPECT_EQ(Far.Divergences[0].Type, DivergenceType::Excess);
  EXPECT_EQ(Far.Divergences[1].Type, DivergenceType::Missed);
}

TEST(Divergence, SizeOver20Percent) {
  Market M;
  const DivergenceResult Over =
      analyze(M, {market(0, 1, Side::Buy, 10)},
              algos(algo("ref", atStep1("buy 13 lots"))));
  ASSERT_EQ(Over.Divergences.size(), 1u);
  const Divergence &D = Over.Divergences[0];
  EXPECT_EQ(D.Type, DivergenceType::Size);
  EXPECT_EQ(D.HumanStep, 1);
  EXPECT_EQ(D.AlgoStep, 1);
  EXPECT_EQ(D.HumanLots, 10);
  EXPECT_EQ(D.AlgoLots, 13);
  EXPECT_EQ(D.HorizonStep, 5);
  EXPECT_EQ(D.Cost.toString(2), "15.00"); // 13 * 5 - 10 * 5

  const DivergenceResult Under =
      analyze(M, {market(0, 1, Side::Buy, 10)},
              algos(algo("ref", atStep1("buy 11 lots"))));
  EXPECT_TRUE(Under.Divergences.empty());
  EXPECT_TRUE(Under.Consensus.empty());
}

TEST(Divergence, OppositeSameStep) {
  Market M;
  const DivergenceResult R =
      analyze(M, {market(0, 1, Side::Buy, 10)},
              algos(algo("ref", atStep1("sell 10 lots"))));

  ASSERT_EQ(R.Divergences.size(), 1u);
  const Divergence &D = R.Divergences[0];
  EXPECT_EQ(D.Type, DivergenceType::Excess);
  EXPECT_EQ(D.HumanStep, 1);
  EXPECT_EQ(D.AlgoStep, 1);
  EXPECT_EQ(D.HumanLots, 10);
  EXPECT_EQ(D.AlgoLots, -10);
  EXPECT_EQ(D.ReasonCode, "алгоритм здесь не действовал бы");
  // Продажа алгоритма при нулевой позиции человека отклоняется движком: V
  // алгоритма 0, V человека 50.
  EXPECT_EQ(D.Cost.toString(2), "-50.00");
  EXPECT_TRUE(std::none_of(
      R.Divergences.begin(), R.Divergences.end(),
      [](const Divergence &x) { return x.Type == DivergenceType::Missed; }));
}

TEST(Divergence, Consensus) {
  Market M;
  const std::vector<AlgorithmInput> Algorithms =
      algos(algo("a", atStep1("buy 10 lots")),
            algo("b", atStep1("buy 10 lots")), algo("c", kSilent));
  const DivergenceResult R =
      analyze(M, {market(0, 1, Side::Buy, 10)}, Algorithms);

  ASSERT_EQ(R.Divergences.size(), 1u);
  EXPECT_EQ(R.Divergences[0].AlgorithmId, "c");
  EXPECT_EQ(R.Divergences[0].Type, DivergenceType::Excess);

  ASSERT_EQ(R.Consensus.size(), 1u);
  const ConsensusItem &Item = R.Consensus[0];
  EXPECT_TRUE(Item.HumanAction);
  EXPECT_EQ(Item.Instrument, 0);
  EXPECT_EQ(Item.Step, 1);
  EXPECT_EQ(Item.OrderSide, Side::Buy);
  EXPECT_EQ(Item.Agreeing, 2);
  EXPECT_EQ(Item.Total, 3);
  EXPECT_EQ(Item.MedianCost.toString(2), "-50.00");
  EXPECT_EQ(R.Independent.size(), 3u);
  // Человек купил, и никто из упомянутых не пропустил его вход: сделок для
  // ФТ-20 нет (сделка не закрыта).
  EXPECT_TRUE(R.Missed.empty());
}

// Упущенные действия группируются по (бумага, шаг, сторона); медиана при чётном
// числе - среднее двух средних.
TEST(Divergence, ConsensusOfMissedActions) {
  Market M;
  const std::vector<AlgorithmInput> Algorithms =
      algos(algo("a", atStep1("buy 10 lots")),
            algo("b", atStep1("buy 20 lots")), algo("c", kSilent));
  const DivergenceResult R = analyze(M, {}, Algorithms);

  ASSERT_EQ(R.Divergences.size(), 2u);
  ASSERT_EQ(R.Consensus.size(), 1u);
  const ConsensusItem &Item = R.Consensus[0];
  EXPECT_FALSE(Item.HumanAction);
  EXPECT_EQ(Item.Step, 1);
  EXPECT_EQ(Item.OrderSide, Side::Buy);
  EXPECT_EQ(Item.Agreeing, 2);
  EXPECT_EQ(Item.Total, 3);
  // Цены ошибок 50 и 100: среднее 75.
  EXPECT_EQ(Item.MedianCost.toString(2), "75.00");
}

// Стоп человека не создаёт расхождения (защитные заявки не участвуют ни у
// человека, ни у алгоритма).
TEST(Divergence, ProtectiveOrdersIgnored) {
  Market M;
  const DivergenceResult OnlyStop =
      analyze(M, {place(0, 1, Side::Buy, OrderType::Stop, 10, dec("120"))},
              algos(algo("ref", kSilent)));
  EXPECT_TRUE(OnlyStop.Divergences.empty());
  EXPECT_TRUE(OnlyStop.Consensus.empty());

  // Покупка и стоп на продажу: расхождение - только у покупки.
  const DivergenceResult WithStop =
      analyze(M,
              {market(0, 1, Side::Buy, 10),
               place(1, 2, Side::Sell, OrderType::Stop, 10, dec("90"))},
              algos(algo("ref", kSilent)));
  ASSERT_EQ(WithStop.Divergences.size(), 1u);
  EXPECT_EQ(WithStop.Divergences[0].HumanStep, 1);

  // Заявка человека с OCO (даже если главная нога лимитная) - защитная связка,
  // не действие.
  OrderSpec LimitSell;
  LimitSell.OrderSide = Side::Sell;
  LimitSell.Type = OrderType::Limit;
  LimitSell.Lots = 10;
  LimitSell.LimitPrice = dec("120");
  OrderSpec StopSell;
  StopSell.OrderSide = Side::Sell;
  StopSell.Type = OrderType::Stop;
  StopSell.Lots = 10;
  StopSell.StopPrice = dec("90");
  JournalEntry Oco;
  Oco.Seq = 1;
  Oco.Step = 2;
  Oco.Command = PlaceCmd{LimitSell, StopSell};
  const std::vector<JournalEntry> WithOco{market(0, 1, Side::Buy, 10), Oco};
  ASSERT_TRUE(replaySession(M.Spec, M.TimelineRef, WithOco, 5, 0)
                  .CommandResults[1]
                  .Accepted);
  const DivergenceResult OcoResult =
      analyze(M, WithOco, algos(algo("ref", kSilent)));
  ASSERT_EQ(OcoResult.Divergences.size(), 1u);
  EXPECT_EQ(OcoResult.Divergences[0].HumanStep, 1);

  // Защитная заявка алгоритма тоже не действие.
  const DivergenceResult AlgoStop =
      analyze(M, {}, algos(algo("ref", atStep1("buy 10 lots stop 120"))));
  EXPECT_TRUE(AlgoStop.Divergences.empty());
}

// Лимитная заявка - действие, как и рыночная.
TEST(Divergence, LimitOrderIsAnAction) {
  Market M;
  const DivergenceResult R = analyze(
      M,
      {place(0, 1, Side::Buy, OrderType::Limit, 10, std::nullopt, dec("99"))},
      algos(algo("ref", kSilent)));
  ASSERT_EQ(R.Divergences.size(), 1u);
  EXPECT_EQ(R.Divergences[0].Type, DivergenceType::Excess);
  EXPECT_EQ(R.Divergences[0].HumanLots, 10);
  // Лимит 99 ниже рынка: не исполнилась, цена ошибки 0.
  EXPECT_EQ(R.Divergences[0].Cost.toString(2), "0.00");
}

// Алгоритм получает портфель человека: продажа возможна только потому, что
// человек уже купил.
TEST(Divergence, AlgorithmSeesHumanPosition) {
  Market M;
  const DivergenceResult R =
      analyze(M, {market(0, 1, Side::Buy, 10)},
              algos(algo("ref", "rule \"r\" when position > 0 and close == 110 "
                                "and close[1] == 110 then sell 10 lots "
                                "because \"выход\"\n")));

  ASSERT_EQ(R.Divergences.size(), 2u);
  const Divergence &Excess = R.Divergences[0];
  EXPECT_EQ(Excess.Type, DivergenceType::Excess);
  EXPECT_EQ(Excess.HorizonStep,
            3); // следующее действие - продажа алгоритма на шаге 3
  EXPECT_EQ(Excess.Cost.toString(2), "-100.00");
  const Divergence &Missed = R.Divergences[1];
  EXPECT_EQ(Missed.Type, DivergenceType::Missed);
  EXPECT_EQ(Missed.AlgoStep, 3);
  EXPECT_EQ(Missed.AlgoLots, -10);
  // Алгоритм продаёт по open шага 4 = 110, close шага 5 = 105: -10 * (105 -
  // 110) = 50.
  EXPECT_EQ(Missed.Cost.toString(2), "50.00");
}

TEST(Divergence, Deterministic) {
  Market M;
  const std::vector<JournalEntry> Journal{market(0, 1, Side::Buy, 10),
                                          market(1, 3, Side::Sell, 10)};
  const std::vector<AlgorithmInput> Algorithms = algos(
      algo("a", atStep1("buy 13 lots")), algo("b", atStep3("sell 10 lots")));
  const DivergenceResult First = analyze(M, Journal, Algorithms);
  const DivergenceResult Second = analyze(M, Journal, Algorithms);
  ASSERT_EQ(First.Divergences.size(), Second.Divergences.size());
  for (std::size_t I = 0; I < First.Divergences.size(); ++I) {
    EXPECT_EQ(First.Divergences[I].Type, Second.Divergences[I].Type);
    EXPECT_EQ(First.Divergences[I].Cost, Second.Divergences[I].Cost);
    EXPECT_EQ(First.Divergences[I].AlgorithmId,
              Second.Divergences[I].AlgorithmId);
  }
  ASSERT_EQ(First.Independent.size(), Second.Independent.size());
  for (std::size_t I = 0; I < First.Independent.size(); ++I) {
    EXPECT_EQ(First.Independent[I].Digest, Second.Independent[I].Digest);
  }
}

// Журнал человека не меняется от разбора, а сам разбор не зависит от числа
// алгоритмов у соседей (каждый отдельно).
TEST(Divergence, AlgorithmsAreIndependent) {
  Market M;
  const std::vector<JournalEntry> Journal{market(0, 1, Side::Buy, 10)};
  const DivergenceResult Alone =
      analyze(M, Journal, algos(algo("a", atStep1("buy 13 lots"))));
  const DivergenceResult Together =
      analyze(M, Journal,
              algos(algo("b", atStep3("buy 10 lots")),
                    algo("a", atStep1("buy 13 lots"))));
  ASSERT_EQ(Alone.Divergences.size(), 1u);
  ASSERT_EQ(Together.Divergences.size(), 2u);
  EXPECT_EQ(Together.Divergences[0].AlgorithmId, "b");
  EXPECT_EQ(Together.Divergences[1].AlgorithmId, "a");
  EXPECT_EQ(Together.Divergences[1].Cost, Alone.Divergences[0].Cost);
}

TEST(Divergence, InvalidInput) {
  Market M;
  const std::vector<AlgorithmInput> One = algos(algo("a", kSilent));
  // lastStep вне шкалы.
  EXPECT_THROW(analyzeDivergences(M.Spec, M.TimelineRef, {}, 6, One),
               std::invalid_argument);
  EXPECT_THROW(analyzeDivergences(M.Spec, M.TimelineRef, {}, -1, One),
               std::invalid_argument);
  // naive.
  SessionSpec Naive = M.Spec;
  Naive.Naive = true;
  EXPECT_THROW(analyzeDivergences(Naive, M.TimelineRef, {}, 5, One),
               std::invalid_argument);
  // Больше пяти алгоритмов и повтор id.
  std::vector<AlgorithmInput> Six;
  for (int I = 0; I < 6; ++I)
    Six.push_back(algo("a" + std::to_string(I), kSilent));
  EXPECT_THROW(analyze(M, {}, Six), std::invalid_argument);
  EXPECT_THROW(analyze(M, {}, algos(algo("a", kSilent), algo("a", kSilent))),
               std::invalid_argument);
  // Журнал за lastStep.
  EXPECT_THROW(
      analyzeDivergences(M.Spec, M.TimelineRef,
                         std::vector<JournalEntry>{market(0, 6, Side::Buy, 1)},
                         5, One),
      std::invalid_argument);
}

// Без алгоритмов сравнивать не с чем.
TEST(Divergence, NoAlgorithms) {
  Market M;
  const DivergenceResult R = analyze(M, {market(0, 1, Side::Buy, 10)}, {});
  EXPECT_TRUE(R.Divergences.empty());
  EXPECT_TRUE(R.Consensus.empty());
  EXPECT_TRUE(R.Independent.empty());
}

// INV-1: действия алгоритма на шагах до t не зависят от баров после t (меняем
// только хвост шкалы).
TEST(Divergence, AlgorithmActionsDoNotDependOnFutureBars) {
  Market Base;
  Market Changed;
  auto [spec, timeline] = core::testing::syntheticTimeline(
      {"100", "100", "110", "110", "50", "300"});
  Changed.Spec = spec;
  Changed.Spec.Costs = CostConfig::zero();
  Changed.TimelineRef = timeline;
  const std::string Rule =
      "rule \"r\" when sma(close, 2) > 104 and close[1] == 100 then buy 10 "
      "lots because \"{sma(close, 2)}\"\n";

  const DivergenceResult A = analyze(Base, {}, algos(algo("ref", Rule)));
  const DivergenceResult B = analyze(Changed, {}, algos(algo("ref", Rule)));
  ASSERT_EQ(A.Divergences.size(), 1u);
  ASSERT_EQ(B.Divergences.size(), 1u);
  EXPECT_EQ(A.Divergences[0].AlgoStep, 2);
  EXPECT_EQ(A.Divergences[0].AlgoStep, B.Divergences[0].AlgoStep);
  EXPECT_EQ(A.Divergences[0].AlgoLots, B.Divergences[0].AlgoLots);
  EXPECT_EQ(A.Divergences[0].ReasonCode, B.Divergences[0].ReasonCode);
  // Цена ошибки смотрит вперёд до горизонта (это разбор постфактум) и потому
  // различается.
  EXPECT_NE(A.Divergences[0].Cost, B.Divergences[0].Cost);
}

// Сессия закончилась раньше конца шкалы: горизонты, V и независимые портфели -
// только по шагам до lastStep.
TEST(Divergence, EarlyFinishUsesOnlyStepsUpToLastStep) {
  Market M;
  Market Changed; // тот же отрезок 0…3, но другой хвост
  auto [spec, timeline] = core::testing::syntheticTimeline(
      {"100", "100", "110", "110", "50", "300"});
  Changed.Spec = spec;
  Changed.Spec.Costs = CostConfig::zero();
  Changed.TimelineRef = timeline;
  const std::int64_t Last = 3;

  const std::vector<JournalEntry> Journal{market(0, 1, Side::Buy, 10)};
  const auto Run = [&](const Market &Market) {
    return analyzeDivergences(
        Market.Spec, Market.TimelineRef, Journal, Last,
        algos(algo("ref", atStep1("buy 13 lots")), algo("silent", kSilent)));
  };
  const DivergenceResult A = Run(M);
  const DivergenceResult B = Run(Changed);

  ASSERT_EQ(A.Divergences.size(), 2u);
  // Размер: горизонт - lastStep, обе покупки по open шага 2 = 100, close(3) =
  // 110: 13 * 10 - 10 * 10 = 30.
  EXPECT_EQ(A.Divergences[0].AlgorithmId, "ref");
  EXPECT_EQ(A.Divergences[0].Type, DivergenceType::Size);
  EXPECT_EQ(A.Divergences[0].HorizonStep, Last);
  EXPECT_EQ(A.Divergences[0].Cost.toString(2), "30.00");
  // Лишнее действие: V человека = 10 * (110 - 100) = 100 на close(lastStep).
  EXPECT_EQ(A.Divergences[1].AlgorithmId, "silent");
  EXPECT_EQ(A.Divergences[1].HorizonStep, Last);
  EXPECT_EQ(A.Divergences[1].Cost.toString(2), "-100.00");

  // Независимый портфель - прогон по тому же отрезку: 1 000 000 - 1300 + 13 *
  // 110 (покупка на шаге 1 исполнена на шаге 2).
  ASSERT_EQ(A.Independent.size(), 2u);
  EXPECT_EQ(A.Independent[0].LastStep, Last);
  EXPECT_EQ(A.Independent[0].Equity.size(), static_cast<std::size_t>(Last + 1));
  EXPECT_EQ(A.Independent[0].FinalEquity.toString(2), "1000130.00");
  EXPECT_EQ(A.Independent[1].FinalEquity.toString(2), "1000000.00");

  // Хвост шкалы за lastStep ни на что не влияет.
  ASSERT_EQ(B.Divergences.size(), A.Divergences.size());
  for (std::size_t I = 0; I < A.Divergences.size(); ++I) {
    EXPECT_EQ(A.Divergences[I].Cost, B.Divergences[I].Cost);
    EXPECT_EQ(A.Divergences[I].HorizonStep, B.Divergences[I].HorizonStep);
    EXPECT_EQ(A.Divergences[I].CostPercent, B.Divergences[I].CostPercent);
  }
  for (std::size_t I = 0; I < A.Independent.size(); ++I) {
    EXPECT_EQ(A.Independent[I].Digest, B.Independent[I].Digest);
  }
}

// Лимитная заявка, которая держится 50 баров, - одно действие, а не пятьдесят.
TEST(Divergence, RepeatedLimitIntentIsOneAction) {
  const std::vector<std::string> Closes(50, "100");
  auto [spec, timeline] = core::testing::syntheticTimeline(Closes);
  Market M;
  M.Spec = spec;
  M.Spec.Costs = CostConfig::zero();
  M.TimelineRef = timeline;
  const DivergenceResult R =
      analyze(M, {},
              algos(algo("ref", "rule \"r\" when true then buy 10 lots limit "
                                "90 because \"ждать 90\"\n")));
  ASSERT_EQ(R.Divergences.size(), 1u);
  EXPECT_EQ(R.Divergences[0].Type, DivergenceType::Missed);
  EXPECT_EQ(R.Divergences[0].AlgoStep, 0);

  // Рыночные заявки исполняются на следующем баре: повтор - новое действие
  // каждый раз (последний шаг - конец сессии).
  const DivergenceResult Market = analyze(
      M, {},
      algos(
          algo("ref", "rule \"r\" when true then buy 1 lots because \"x\"\n")));
  EXPECT_EQ(Market.Divergences.size(), 49u);
}

// Подача и замена на одном шаге - одно действие, без ложного расхождения
// «размер».
TEST(Divergence, PlaceThenReplaceOnSameStepIsOneAction) {
  Market M;
  JournalEntry Replace;
  Replace.Seq = 1;
  Replace.Step = 1;
  OrderSpec Replacement;
  Replacement.OrderSide = Side::Buy;
  Replacement.Type = OrderType::Market;
  Replacement.Lots = 10;
  Replace.Command = ReplaceCmd{0, PlaceCmd{Replacement, std::nullopt}};
  const std::vector<JournalEntry> Journal{market(0, 1, Side::Buy, 10), Replace};
  ASSERT_TRUE(replaySession(M.Spec, M.TimelineRef, Journal, 5, 0)
                  .CommandResults[1]
                  .Accepted);

  const DivergenceResult R =
      analyze(M, Journal, algos(algo("ref", atStep1("buy 10 lots"))));
  EXPECT_TRUE(R.Divergences.empty());

  // Замена заявки, поданной на прошлом шаге, - новое действие; прежнее остаётся
  // и сопоставляется само по себе.
  Replace.Step = 2;
  const std::vector<JournalEntry> Later{
      place(0, 1, Side::Buy, OrderType::Limit, 10, std::nullopt, dec("90")),
      Replace};
  ASSERT_TRUE(replaySession(M.Spec, M.TimelineRef, Later, 5, 0)
                  .CommandResults[1]
                  .Accepted);
  const DivergenceResult R2 =
      analyze(M, Later, algos(algo("ref", atStep1("buy 10 lots"))));
  ASSERT_EQ(R2.Divergences.size(), 1u);
  EXPECT_EQ(R2.Divergences[0].HumanStep, 2);
  EXPECT_EQ(R2.Divergences[0].Type, DivergenceType::Excess);
}

// Медиана консенсуса - только по алгоритмам, которые «здесь не действовали бы»;
// «размер» и «момент» в неё не входят.
TEST(Divergence, ConsensusMedianOnlyOverNonAgreeingAlgorithms) {
  Market M;
  const DivergenceResult R =
      analyze(M, {market(0, 1, Side::Buy, 10)},
              algos(algo("a", atStep1("buy 13 lots")),
                    algo("b", atStep1("buy 13 lots")), algo("c", kSilent)));
  ASSERT_EQ(R.Divergences.size(),
            3u); // два «размера» по 15.00 и одно «лишнее» -50.00
  ASSERT_EQ(R.Consensus.size(), 1u);
  EXPECT_EQ(R.Consensus[0].Agreeing, 2);
  EXPECT_EQ(R.Consensus[0].Total, 3);
  EXPECT_EQ(R.Consensus[0].MedianCost.toString(2), "-50.00");
}

// Прогрев: алгоритм на месте человека видит историю до шага 0 (как и в
// независимом прогоне).
TEST(Divergence, WarmupHistoryIsVisibleToAlgorithm) {
  Market M;
  // История: 90 и 95 до шага 0. Правило срабатывает только на шаге 0 и только с
  // историей: close 100, close[1] 95.
  constexpr std::int64_t kDayMs = 86'400'000;
  constexpr std::int64_t kStart =
      1'704'747'600'000; // 2024-01-09 00:00 МСК - шаг 0 рынка
  std::vector<Bar> History;
  for (int I = 0; I < 2; ++I) {
    Bar B;
    B.TsOpenMs = kStart - (2 - I) * kDayMs;
    B.AvailableAtMs = B.TsOpenMs + kDayMs;
    B.Open = B.Close = dec(I == 0 ? "90" : "95");
    B.High = B.Close + dec("0.5");
    B.Low = B.Close - dec("0.5");
    B.Volume = 1'000'000;
    History.push_back(B);
  }
  const Timeline Before =
      Timeline::build({core::testing::syntheticInstrument()}, {History}, {});
  const std::string Rule = "rule \"r\" when close == 100 and close[1] == 95 "
                           "then buy 10 lots because \"после 95\"\n";

  const DivergenceResult Plain = analyze(M, {}, algos(algo("ref", Rule)));
  EXPECT_TRUE(Plain.Divergences.empty());

  M.TimelineRef =
      std::make_shared<const Timeline>(M.TimelineRef->withWarmup(Before, 2));
  const DivergenceResult Warm = analyze(M, {}, algos(algo("ref", Rule)));
  ASSERT_EQ(Warm.Divergences.size(), 1u);
  EXPECT_EQ(Warm.Divergences[0].Type, DivergenceType::Missed);
  EXPECT_EQ(Warm.Divergences[0].AlgoStep, 0);
  EXPECT_EQ(Warm.Divergences[0].ReasonCode, "после 95");
}
