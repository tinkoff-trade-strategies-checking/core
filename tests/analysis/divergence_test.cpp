#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/analysis/divergence.hpp"
#include "core/engine/digest.hpp"
#include "core/market/date_utils.hpp"
#include "core/strategy/strategy_runner.hpp"

namespace core {
namespace {

struct SimpleBar {
  std::string Open;
  std::string High;
  std::string Low;
  std::string Close;
  std::int64_t Volume = 10000;
  std::int32_t DayOffset = -1;
};

std::shared_ptr<const Timeline>
makeTimeline(const std::vector<SimpleBar> &BarsSpec,
             const std::string &PriceStep = "0.01", std::int64_t LotSize = 1,
             const std::vector<SimpleBar> &WarmupSpec = {}) {
  std::vector<Instrument> Instruments{
      Instrument{"A", "TEST", LotSize, Decimal::parse(PriceStep)}};
  std::vector<Bar> Bars;
  std::vector<TradingDay> Calendar;
  const std::int64_t BaseMs = moscowDayStartMs(20240201);

  std::int64_t Idx = 0;
  auto PushBar = [&](const SimpleBar &S, std::vector<Bar> &Out) {
    const std::int64_t OpenMs =
        S.DayOffset >= 0 ? BaseMs + S.DayOffset * kDayMs + Idx * 60000
                         : BaseMs + Idx * 60000;
    ++Idx;
    Bar B;
    B.TsOpenMs = OpenMs;
    B.AvailableAtMs = OpenMs + 60000;
    B.Open = Decimal::parse(S.Open);
    B.High = Decimal::parse(S.High);
    B.Low = Decimal::parse(S.Low);
    B.Close = Decimal::parse(S.Close);
    B.Volume = S.Volume;
    B.Value = B.Close.times(B.Volume);
    Out.push_back(B);
    const std::int32_t Ymd = moscowYmd(OpenMs);
    if (Calendar.empty() || Calendar.back().Yyyymmdd != Ymd) {
      Calendar.push_back(TradingDay{Ymd, true});
    }
  };

  std::vector<Bar> WarmupBars;
  for (const SimpleBar &S : WarmupSpec) {
    PushBar(S, WarmupBars);
  }
  for (const SimpleBar &S : BarsSpec) {
    PushBar(S, Bars);
  }
  Timeline MainTl = Timeline::build(Instruments, {Bars}, Calendar);
  if (!WarmupBars.empty()) {
    const Timeline Hist = Timeline::build(Instruments, {WarmupBars}, Calendar);
    MainTl =
        MainTl.withWarmup(Hist, static_cast<std::int64_t>(WarmupBars.size()));
  }
  return std::make_shared<const Timeline>(std::move(MainTl));
}

RuoniaSeries makeRuonia() {
  RuoniaSeries Series;
  Series.Rates.push_back(DailyRate{20240131, Decimal::parse("15.0")});
  Series.Rates.push_back(DailyRate{20240201, Decimal::parse("15.0")});
  Series.Rates.push_back(DailyRate{20240202, Decimal::parse("15.0")});
  Series.Rates.push_back(DailyRate{20240205, Decimal::parse("15.0")});
  return Series;
}

SessionSpec makeSpec(const std::shared_ptr<const Timeline> &Tl,
                     const std::string &CashStr = "100000",
                     CostConfig Costs = CostConfig::zero()) {
  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs = Costs;
  Spec.InitialCash = Decimal::parse(CashStr);
  return Spec;
}

JournalEntry placeMarket(std::int64_t Seq, std::int64_t Step, Side OrderSide,
                         std::int64_t Lots, int Instr = 0) {
  OrderSpec Spec;
  Spec.Instrument = Instr;
  Spec.OrderSide = OrderSide;
  Spec.Type = OrderType::Market;
  Spec.Lots = Lots;
  return JournalEntry{Seq, Step, PlaceCmd{Spec, std::nullopt}, "", ""};
}

JournalEntry placeLimit(std::int64_t Seq, std::int64_t Step, Side OrderSide,
                        std::int64_t Lots, const std::string &LimitStr,
                        int Instr = 0) {
  OrderSpec Spec;
  Spec.Instrument = Instr;
  Spec.OrderSide = OrderSide;
  Spec.Type = OrderType::Limit;
  Spec.Lots = Lots;
  Spec.LimitPrice = Decimal::parse(LimitStr);
  return JournalEntry{Seq, Step, PlaceCmd{Spec, std::nullopt}, "", ""};
}

TEST(Divergence, ValidationRejectsInvalidInputs) {
  const auto Tl = makeTimeline({
      SimpleBar{"100", "105", "95", "100"},
      SimpleBar{"100", "105", "95", "100"},
  });
  const SessionSpec Spec = makeSpec(Tl);
  const std::vector<JournalEntry> EmptyHuman;
  const std::string SimpleSrc =
      "rule \"b\" when false then buy 1 lots market because \"ok\"\n";

  EXPECT_THROW(analyzeDivergences(Spec, nullptr, EmptyHuman, 1, {}),
               std::invalid_argument);
  EXPECT_THROW(analyzeDivergences(Spec, Tl, EmptyHuman, -1, {}),
               std::invalid_argument);
  EXPECT_THROW(analyzeDivergences(Spec, Tl, EmptyHuman, 2, {}),
               std::invalid_argument);

  std::vector<AlgorithmInput> SixAlgos;
  for (int I = 0; I < 6; ++I) {
    SixAlgos.push_back({"a" + std::to_string(I), SimpleSrc, {}});
  }
  EXPECT_THROW(analyzeDivergences(Spec, Tl, EmptyHuman, 1, SixAlgos),
               std::invalid_argument);

  const std::vector<AlgorithmInput> DupIds{{"a1", SimpleSrc, {}},
                                           {"a1", SimpleSrc, {}}};
  EXPECT_THROW(analyzeDivergences(Spec, Tl, EmptyHuman, 1, DupIds),
               std::invalid_argument);

  const std::vector<AlgorithmInput> BadSyntax{{"bad", "not valid dsl", {}}};
  EXPECT_THROW(analyzeDivergences(Spec, Tl, EmptyHuman, 1, BadSyntax),
               std::invalid_argument);

  const std::vector<AlgorithmInput> LookaheadErr{
      {"la",
       "rule \"e\" when close[-1] > 0 then buy 1 lots market because \"la\"\n",
       {}}};
  EXPECT_THROW(analyzeDivergences(Spec, Tl, EmptyHuman, 1, LookaheadErr),
               std::invalid_argument);

  const std::vector<JournalEntry> BadStep{
      placeMarket(0, 1, Side::Buy, 1),
      placeMarket(1, 0, Side::Buy, 1),
  };
  EXPECT_THROW(analyzeDivergences(Spec, Tl, BadStep, 1, {}),
               std::invalid_argument);

  const std::vector<JournalEntry> BadSeq{
      placeMarket(1, 0, Side::Buy, 1),
      placeMarket(1, 1, Side::Buy, 1),
  };
  EXPECT_THROW(analyzeDivergences(Spec, Tl, BadSeq, 1, {}),
               std::invalid_argument);

  const DivergenceResult ZeroRes =
      analyzeDivergences(Spec, Tl, EmptyHuman, 1, {});
  EXPECT_TRUE(ZeroRes.Divergences.empty());
  EXPECT_TRUE(ZeroRes.Missed.empty());
  EXPECT_TRUE(ZeroRes.Consensus.empty());
  EXPECT_TRUE(ZeroRes.Independent.empty());
}

TEST(Divergence, ExactMatchAndSizeDivergenceAndCostPercent) {
  const auto Tl = makeTimeline({
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "121", "99", "120"},
      SimpleBar{"120", "125", "119", "120"},
  });
  const SessionSpec Spec = makeSpec(Tl, "10000");
  const std::string AlgoSrc = R"dsl(
rule "buy5"
  when position == 0 and close == 100
  then buy 5 lots market
  because "вход на 5 лотов"
)dsl";

  // 4 против 5: |4-5|=1, 5*1 = 5 <= 5 -> точное совпадение, расхождений нет.
  const std::vector<JournalEntry> ExactHuman{placeMarket(0, 0, Side::Buy, 4)};
  const DivergenceResult ExactRes =
      analyzeDivergences(Spec, Tl, ExactHuman, 2, {{"a1", AlgoSrc, {}}});
  EXPECT_TRUE(ExactRes.Divergences.empty());
  EXPECT_TRUE(ExactRes.Consensus.empty());

  // 3 против 5: |3-5|=2, 5*2 = 10 > 5 -> Size.
  // На шаге 1 open=100, к горизонту step=2 close=120:
  // V_human = 3*(120-100) = 60, V_algo = 5*(120-100) = 100 -> Cost = 40.00.
  // Equity[0] = 10000 -> CostPercent = 40 * 100 / 10000 = 0.40.
  const std::vector<JournalEntry> SizeHuman{placeMarket(0, 0, Side::Buy, 3)};
  const DivergenceResult SizeRes =
      analyzeDivergences(Spec, Tl, SizeHuman, 2, {{"a1", AlgoSrc, {}}});
  ASSERT_EQ(SizeRes.Divergences.size(), 1u);
  const Divergence &D = SizeRes.Divergences[0];
  EXPECT_EQ(D.Seq, 0);
  EXPECT_EQ(D.Type, DivergenceType::Size);
  EXPECT_EQ(D.HumanStep, 0);
  EXPECT_EQ(D.AlgoStep, 0);
  EXPECT_EQ(D.HumanLots, 3);
  EXPECT_EQ(D.AlgoLots, 5);
  EXPECT_EQ(D.HorizonStep, 2);
  EXPECT_EQ(D.Cost, Decimal::parse("40"));
  EXPECT_EQ(D.CostPercent, Decimal::parse("0.4"));
  EXPECT_EQ(D.ReasonCode, "вход на 5 лотов");
  EXPECT_EQ(D.HumanOrderType, OrderType::Market);
  EXPECT_EQ(D.AlgoOrderType, OrderType::Market);
}

TEST(Divergence, TimingWindowTieBreakAndOutsideWindow) {
  const auto Tl = makeTimeline({
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"101", "102", "100", "101"},
      SimpleBar{"102", "103", "101", "102"},
      SimpleBar{"103", "104", "102", "103"},
      SimpleBar{"104", "105", "103", "104"},
      SimpleBar{"105", "106", "104", "105"},
  });
  const SessionSpec Spec = makeSpec(Tl, "10000");

  // Алгоритм действует на шаге 0 и шаге 4 (расстояние 2 от шага 2); человек
  // действует на шаге 2 -> выбирается более ранний шаг 0 (Timing, At = 2).
  const std::string TieSrc = R"dsl(
rule "buy_at_0_and_4"
  when close == 100 or close == 104
  then buy 1 lots market
  because "вход на {close}"
)dsl";
  const std::vector<JournalEntry> Human{placeMarket(0, 2, Side::Buy, 1)};
  const DivergenceResult TieRes =
      analyzeDivergences(Spec, Tl, Human, 5, {{"a1", TieSrc, {}}});
  ASSERT_EQ(TieRes.Divergences.size(), 2u);
  EXPECT_EQ(TieRes.Divergences[0].Type, DivergenceType::Timing);
  EXPECT_EQ(TieRes.Divergences[0].HumanStep, 2);
  EXPECT_EQ(TieRes.Divergences[0].AlgoStep, 0);
  EXPECT_EQ(TieRes.Divergences[0].HorizonStep, 4);
  EXPECT_EQ(TieRes.Divergences[1].Type, DivergenceType::Missed);
  EXPECT_EQ(TieRes.Divergences[1].AlgoStep, 4);

  // Расстояние 4 (> W=3): Timing не срабатывает -> Excess + Missed.
  const std::string FarSrc = R"dsl(
rule "far"
  when close == 100
  then buy 1 lots market
  because "далеко"
)dsl";
  const std::vector<JournalEntry> FarHuman{placeMarket(0, 4, Side::Buy, 1)};
  const DivergenceResult FarRes =
      analyzeDivergences(Spec, Tl, FarHuman, 5, {{"a1", FarSrc, {}}});
  ASSERT_EQ(FarRes.Divergences.size(), 2u);
  EXPECT_EQ(FarRes.Divergences[0].Type, DivergenceType::Missed);
  EXPECT_EQ(FarRes.Divergences[0].AlgoStep, 0);
  EXPECT_EQ(FarRes.Divergences[1].Type, DivergenceType::Excess);
  EXPECT_EQ(FarRes.Divergences[1].HumanStep, 4);
}

TEST(Divergence, OppositeActionsOnSameStepProduceSingleExcess) {
  const auto Tl = makeTimeline({
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"105", "106", "104", "105"},
      SimpleBar{"108", "109", "107", "108"},
      SimpleBar{"110", "111", "109", "110"},
      SimpleBar{"110", "111", "109", "110"},
      SimpleBar{"120", "121", "119", "120"},
  });
  const SessionSpec Spec = makeSpec(Tl, "10000");
  // Шаг 0: человек покупает 2 лота (|4-0|=4 > W=3, чтобы не совпало по Timing).
  // Шаг 4: человек продаёт 1 лот, а алгоритм покупает 2 лота -> одно Excess!
  // Горизонт = 6 (close = 120), исполнения на шаге 5 @ 110:
  // V_human = -1*(120-110) = -10, V_algo = +2*(120-110) = +20 -> Cost = 30.00.
  const std::string AlgoSrc = R"dsl(
rule "buy_step4"
  when position == 2 and close == 110
  then buy 2 lots market
  because "докупка"
)dsl";
  const std::vector<JournalEntry> Human{
      placeMarket(0, 0, Side::Buy, 2),
      placeMarket(1, 4, Side::Sell, 1),
  };
  const DivergenceResult Res =
      analyzeDivergences(Spec, Tl, Human, 6, {{"a1", AlgoSrc, {}}});
  ASSERT_EQ(Res.Divergences.size(), 2u);
  EXPECT_EQ(Res.Divergences[0].Type, DivergenceType::Excess);
  EXPECT_EQ(Res.Divergences[0].HumanStep, 0);
  EXPECT_EQ(Res.Divergences[1].Type, DivergenceType::Excess);
  EXPECT_EQ(Res.Divergences[1].HumanStep, 4);
  EXPECT_EQ(Res.Divergences[1].AlgoStep, 4);
  EXPECT_EQ(Res.Divergences[1].HumanLots, -1);
  EXPECT_EQ(Res.Divergences[1].AlgoLots, 2);
  EXPECT_EQ(Res.Divergences[1].Cost, Decimal::parse("30"));
  EXPECT_EQ(Res.Divergences[1].ReasonCode, "алгоритм здесь не действовал бы");
}

TEST(Divergence, HumanSameStepCancelReplaceNetZeroAndMixedTypes) {
  const auto Tl = makeTimeline({
      SimpleBar{"100", "105", "95", "100"},
      SimpleBar{"100", "105", "95", "100"},
      SimpleBar{"100", "105", "95", "100"},
      SimpleBar{"100", "105", "95", "100"},
  });
  const SessionSpec Spec = makeSpec(Tl, "100000");

  OrderSpec StopSpec;
  StopSpec.Instrument = 0;
  StopSpec.OrderSide = Side::Buy;
  StopSpec.Type = OrderType::Stop;
  StopSpec.Lots = 1;
  StopSpec.StopPrice = Decimal::parse("102");

  // Шаг 0: buy 2 market + cancel -> нет действия; затем buy 3 market replace
  // на limit 2 -> одно Limit-действие (+2).
  // Шаг 1: buy 2 market + sell 2 limit -> нетто 0 -> нет действия.
  // Шаг 2: buy 3 market + sell 1 limit -> нетто +2, смешанный тип -> nullopt.
  // Стоп-заявка на шаге 0 игнорируется.
  const PlaceCmd RepLim =
      std::get<PlaceCmd>(placeLimit(0, 0, Side::Buy, 2, "98").Command);
  const std::vector<JournalEntry> Human{
      placeMarket(0, 0, Side::Buy, 2),
      JournalEntry{1, 0, CancelCmd{0, Reason::CancelledByUser}, "", ""},
      placeMarket(2, 0, Side::Buy, 3),
      JournalEntry{3, 0, ReplaceCmd{4, RepLim}, "", ""},
      JournalEntry{4, 0, PlaceCmd{StopSpec, std::nullopt}, "", ""},
      placeMarket(5, 1, Side::Buy, 2),
      placeLimit(6, 1, Side::Sell, 2, "102"),
      placeMarket(7, 2, Side::Buy, 3),
      placeLimit(8, 2, Side::Sell, 1, "102"),
  };

  const std::string NoOpSrc =
      "rule \"noop\" when false then buy 1 lots market because \"no\"\n";
  const DivergenceResult Res1 =
      analyzeDivergences(Spec, Tl, Human, 3, {{"a1", NoOpSrc, {}}});
  ASSERT_EQ(Res1.Divergences.size(), 2u);
  EXPECT_EQ(Res1.Divergences[0].HumanStep, 0);
  EXPECT_EQ(Res1.Divergences[0].HumanLots, 2);
  EXPECT_EQ(Res1.Divergences[0].HumanOrderType, OrderType::Limit);
  EXPECT_EQ(Res1.Divergences[1].HumanStep, 2);
  EXPECT_EQ(Res1.Divergences[1].HumanLots, 2);
  EXPECT_EQ(Res1.Divergences[1].HumanOrderType, std::nullopt);
}

TEST(Divergence, VirtualOrdersLimitDedupTouchVsCrossAndDayExpiry) {
  // Шаг 0 (день 0): лимит buy 1 @ 95 (GTC).
  // Шаг 1 (день 0): Low == 95 (касание не исполняет) -> заявка остаётся в
  // виртуальном наборе, повтор на шаге 1 дедуплицируется!
  // Шаг 2 (день 1): Low == 94 < 95 -> заявка срабатывает и закрывается ->
  // повтор на шаге 2 даёт новое действие!
  const auto Tl = makeTimeline({
      SimpleBar{"100", "101", "99", "100", 10000, 0},
      SimpleBar{"100", "101", "95", "100", 10000, 0},
      SimpleBar{"100", "101", "94", "100", 10000, 1},
      SimpleBar{"100", "101", "99", "100", 10000, 1},
  });
  const SessionSpec Spec = makeSpec(Tl, "10000");
  const std::string LimitGtc = R"dsl(
rule "lim"
  when true
  then buy 1 lots limit 95
  because "лимит 95"
)dsl";
  const RuoniaSeries Ruonia = makeRuonia();
  const DivergenceResult GtcRes = analyzeDivergences(
      Spec, Tl, {}, 3, {{"a1", LimitGtc, {}}}, std::nullopt, Ruonia, 20240131);
  ASSERT_EQ(GtcRes.Divergences.size(), 2u);
  EXPECT_EQ(GtcRes.Divergences[0].AlgoStep, 0);
  EXPECT_EQ(GtcRes.Divergences[1].AlgoStep, 2);

  // Вариант Day: шаг 0 — день 0, на шаге 1 (последний шаг дня 0) заявка с
  // шага 0 уже истекла по сроку day, поэтому на шаге 1 правило выставляет
  // новую заявку (которая живёт весь день 1, на шаге 2 дедуплицируется).
  const std::string LimitDay = R"dsl(
rule "lim_day"
  when true
  then buy 1 lots limit 90 day
  because "дневной лимит"
)dsl";
  const DivergenceResult DayRes = analyzeDivergences(
      Spec, Tl, {}, 3, {{"a1", LimitDay, {}}}, std::nullopt, Ruonia, 20240131);
  ASSERT_EQ(DayRes.Divergences.size(), 2u);
  EXPECT_EQ(DayRes.Divergences[0].AlgoStep, 0);
  EXPECT_EQ(DayRes.Divergences[1].AlgoStep, 1);
}

TEST(Divergence, VirtualOrdersLiquidityLimitPartialAndAutoCancelZeroPos) {
  // LiquidityShare = 0.1, VolumeLots = 10 -> cap = 1 < 3.
  // Шаг 0: алгоритм ставит limit buy 3 @ 95.
  // Шаг 1: бар Open=94 <= 95 -> частичное исполнение 1 лота, остаток 2 остаётся
  // активным (повтор на шаге 1 дедуплицируется!).
  // Шаг 2: VolumeLots = 30 -> cap = 3 >= 2 -> остаток полностью исполняется,
  // правило на шаге 2 создаёт новую заявку!
  const auto Tl = makeTimeline({
      SimpleBar{"100", "101", "99", "100", 10},
      SimpleBar{"94", "101", "93", "100", 10},
      SimpleBar{"94", "101", "93", "100", 30},
      SimpleBar{"100", "101", "99", "100", 30},
  });
  CostConfig Costs = CostConfig::zero();
  Costs.LiquidityShare = Decimal::parse("0.1");
  const SessionSpec Spec = makeSpec(Tl, "100000", Costs);
  const std::string Src = R"dsl(
rule "liq"
  when true
  then buy 3 lots limit 95
  because "частичная ликвидность"
)dsl";
  const DivergenceResult Res =
      analyzeDivergences(Spec, Tl, {}, 3, {{"a1", Src, {}}});
  ASSERT_EQ(Res.Divergences.size(), 2u);
  EXPECT_EQ(Res.Divergences[0].AlgoStep, 0);
  EXPECT_EQ(Res.Divergences[1].AlgoStep, 2);

  // Автоснятие виртуальной Sell-заявки при position == 0:
  // Шаг 1: position == 1 -> алгоритм ставит sell limit 150 (совпадает с
  // человеком на шаге 1). Шаг 2: position == 0 -> заявка с шага 1 снимается.
  // Шаг 3: position снова == 1 -> создаётся новое действие (Missed на шаге 3).
  const auto AutoTl = makeTimeline({
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "99", "100"},
  });
  const SessionSpec AutoSpec = makeSpec(AutoTl, "100000");
  const std::vector<JournalEntry> AutoHuman{
      placeMarket(0, 0, Side::Buy, 1),
      placeMarket(1, 1, Side::Sell, 1),
      placeMarket(2, 2, Side::Buy, 1),
  };
  const std::string SellSrc = R"dsl(
rule "sell_high"
  when position > 0
  then sell 1 lots limit 150
  because "продажа дорого"
)dsl";
  const DivergenceResult AutoRes =
      analyzeDivergences(AutoSpec, AutoTl, AutoHuman, 4, {{"a1", SellSrc, {}}});
  ASSERT_EQ(AutoRes.Divergences.size(), 3u);
  EXPECT_EQ(AutoRes.Divergences[0].Type, DivergenceType::Excess);
  EXPECT_EQ(AutoRes.Divergences[0].HumanStep, 0);
  EXPECT_EQ(AutoRes.Divergences[1].Type, DivergenceType::Excess);
  EXPECT_EQ(AutoRes.Divergences[1].HumanStep, 2);
  EXPECT_EQ(AutoRes.Divergences[2].Type, DivergenceType::Missed);
  EXPECT_EQ(AutoRes.Divergences[2].AlgoStep, 3);
  EXPECT_EQ(AutoRes.Divergences[2].AlgoLots, -1);
}

TEST(Divergence, VirtualOrdersRejectPreservesOldAndCancelClearsSameBar) {
  const auto Tl = makeTimeline({
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "99", "100"},
  });
  // На шаге 0: close=91 -> 1 лот limit @ 50 (Accepted).
  // На шаге 1: close=120 -> 30 лотов -> > 1000 руб (InsufficientCash по INV-3)
  // -> не становится действием и не снимает заявку шага 0.
  // На шаге 2: снова 1 лот limit @ 50 -> дедуплицируется со стоящей заявкой.
  const SessionSpec Spec = makeSpec(Tl, "1000");
  const auto TlVar = makeTimeline({
      SimpleBar{"91", "92", "90", "91"},
      SimpleBar{"120", "121", "90", "120"},
      SimpleBar{"91", "92", "90", "91"},
      SimpleBar{"92", "93", "90", "92"},
  });
  const std::string ReplaceSrc = R"dsl(
rule "lim"
  when true
  then buy (close - 90) lots limit 50
  because "при close={close}"
)dsl";
  const DivergenceResult RepRes = analyzeDivergences(
      makeSpec(TlVar, "1000"), TlVar, {}, 3, {{"a1", ReplaceSrc, {}}});
  // Шаг 0: buy 1 @ 50 -> Missed (AlgoLots=1).
  // Шаг 1: buy 30 @ 50 -> > 1000 руб -> Rejected (не действие, старая жива).
  // Шаг 2: buy 1 @ 50 -> дедуплицировано со стоящей от шага 0!
  // Шаг 3: последний шаг (== LastStep), действия не вычисляются.
  ASSERT_EQ(RepRes.Divergences.size(), 1u);
  EXPECT_EQ(RepRes.Divergences[0].AlgoStep, 0);
  EXPECT_EQ(RepRes.Divergences[0].AlgoLots, 1);

  // Cancel на том же баре после buy снимает и только что созданную заявку.
  const std::string CancelSameBarSrc = R"dsl(
rule "buy1"
  when true
  then buy 1 lots market
  because "вход"

rule "cancel_all"
  when true
  then cancel
  because "отмена"
)dsl";
  const DivergenceResult CancelRes =
      analyzeDivergences(Spec, Tl, {}, 3, {{"a1", CancelSameBarSrc, {}}});
  EXPECT_TRUE(CancelRes.Divergences.empty());
}

TEST(Divergence, CollapseDuplicatesNetZeroAndJoinBecause) {
  const auto Tl = makeTimeline({
      SimpleBar{"100", "105", "95", "100"},
      SimpleBar{"100", "105", "95", "100"},
  });
  const SessionSpec Spec = makeSpec(Tl, "100000");

  // Два правила с одинаковым (Side, Type, Lots, LimitPrice, Tif) -> схлопывание
  // с приоритетом первого Because; третье правило другого типа (Limit) ->
  // объединение Because через "; ", AlgoLots = 3, AlgoOrderType = nullopt.
  const std::string CollapseSrc = R"dsl(
rule "r1"
  when true
  then buy 1 lots market
  because "первая причина"

rule "r2"
  when true
  then buy 1 lots market
  because "дубль"

rule "r3"
  when true
  then buy 2 lots limit 95
  because "вторая причина"
)dsl";
  const DivergenceResult Res =
      analyzeDivergences(Spec, Tl, {}, 1, {{"a1", CollapseSrc, {}}});
  ASSERT_EQ(Res.Divergences.size(), 1u);
  EXPECT_EQ(Res.Divergences[0].AlgoLots, 3);
  EXPECT_EQ(Res.Divergences[0].AlgoOrderType, std::nullopt);
  EXPECT_EQ(Res.Divergences[0].ReasonCode, "первая причина; вторая причина");

  // Противоположные правила с суммой q_a == 0 -> нет действия.
  const std::string NetZeroSrc = R"dsl(
rule "b"
  when true
  then buy 2 lots market
  because "купить"

rule "s"
  when true
  then sell 2 lots limit 105
  because "продать"
)dsl";
  // Человек уже держит 2 лота, чтобы sell прошёл валидацию INV-3.
  const auto TlPos = makeTimeline({
      SimpleBar{"100", "105", "95", "100"},
      SimpleBar{"100", "105", "95", "100"},
      SimpleBar{"100", "105", "95", "100"},
  });
  const std::vector<JournalEntry> HumanHold{placeMarket(0, 0, Side::Buy, 2)};
  // На шаге 0 у человека 0 лотов (sell отклонён -> buy 2 market на шаге 0),
  // на шаге 1 у человека 2 лота -> buy 2 + sell 2 => q_a == 0 -> нет действия!
  const DivergenceResult NetZeroRes = analyzeDivergences(
      makeSpec(TlPos, "100000"), TlPos, HumanHold, 2, {{"a1", NetZeroSrc, {}}});
  EXPECT_TRUE(NetZeroRes.Divergences.empty());
}

TEST(Divergence, MissedProfitableTradeConsensusAndSeq) {
  const auto Tl = makeTimeline({
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "112", "99", "110"},
      SimpleBar{"120", "121", "119", "120"},
      SimpleBar{"120", "121", "119", "120"},
  });
  const SessionSpec Spec = makeSpec(Tl, "10000");

  // Человек: buy 2 @ шаг 0 (исполнение @ 100 на шаге 1, горизонт = 1,
  // close(1)=110 -> V_human = 2*(110-100) = 20 -> Cost = -20),
  // sell 2 @ шаг 1 (исполнение @ 120 на шаге 2) -> NetPnl = +40.
  const std::vector<JournalEntry> Human{
      placeMarket(0, 0, Side::Buy, 2),
      placeMarket(1, 1, Side::Sell, 2),
  };
  const std::string NeverSrc =
      "rule \"no\" when false then buy 1 lots market because \"no\"\n";
  const std::string SellOnlySrc =
      "rule \"s\" when position > 0 then sell 2 lots market because \"s\"\n";

  const DivergenceResult Res = analyzeDivergences(
      Spec, Tl, Human, 3, {{"a1", NeverSrc, {}}, {"a2", SellOnlySrc, {}}});

  // a1: Excess(step 0, seq=0), Excess(step 1, seq=1).
  // a2: Excess(step 0, seq=2); на шаге 1 совпадает с sell человека!
  ASSERT_EQ(Res.Divergences.size(), 3u);
  EXPECT_EQ(Res.Divergences[0].Seq, 0);
  EXPECT_EQ(Res.Divergences[1].Seq, 1);
  EXPECT_EQ(Res.Divergences[2].Seq, 2);

  // ФТ-20: вход на шаге 0 — Excess у обоих алгоритмов (seq 0 и 2).
  ASSERT_EQ(Res.Missed.size(), 1u);
  const MissedProfitableTrade &M = Res.Missed[0];
  EXPECT_EQ(M.Seq, 0);
  EXPECT_EQ(M.EntryStep, 1);
  EXPECT_EQ(M.ExitStep, 2);
  EXPECT_EQ(M.Lots, 2);
  EXPECT_EQ(M.NetPnl, Decimal::parse("40"));
  EXPECT_EQ(M.EntryPrice, Decimal::parse("100"));
  EXPECT_EQ(M.ExitPrice, Decimal::parse("120"));
  ASSERT_EQ(M.AlgorithmIds.size(), 2u);
  EXPECT_EQ(M.AlgorithmIds[0], "a1");
  EXPECT_EQ(M.AlgorithmIds[1], "a2");
  ASSERT_EQ(M.DivergenceSeqs.size(), 2u);
  EXPECT_EQ(M.DivergenceSeqs[0], 0);
  EXPECT_EQ(M.DivergenceSeqs[1], 2);

  // Консенсус: шаг 0 (Agreeing=0/2, чётная медиана Cost = (-20 + -20)/2 = -20);
  // шаг 1 (Agreeing=1/2, нечётная медиана Cost = 0).
  ASSERT_EQ(Res.Consensus.size(), 2u);
  EXPECT_TRUE(Res.Consensus[0].HumanAction);
  EXPECT_EQ(Res.Consensus[0].Step, 0);
  EXPECT_EQ(Res.Consensus[0].Agreeing, 0);
  EXPECT_EQ(Res.Consensus[0].Total, 2);
  EXPECT_EQ(Res.Consensus[0].MedianCost, Decimal::parse("-20"));
  EXPECT_EQ(Res.Consensus[1].Step, 1);
  EXPECT_EQ(Res.Consensus[1].Agreeing, 1);
  EXPECT_EQ(Res.Consensus[1].Total, 2);
}

TEST(Divergence, EarlyStopInv1IsolationAndIndependentPortfolios) {
  const auto BuildTl = [&](const std::string &TailClose) {
    return makeTimeline({
        SimpleBar{"100", "101", "99", "100"},
        SimpleBar{"100", "101", "99", "100"},
        SimpleBar{"110", "111", "109", "110"},
        SimpleBar{"120", "121", "119", TailClose},
    });
  };
  const auto Tl1 = BuildTl("130");
  const auto Tl2 = BuildTl("999");

  const std::string AlgoSrc = R"dsl(
rule "buy1"
  when position == 0
  then buy 1 lots market
  because "вход"
)dsl";
  const std::vector<JournalEntry> Human{placeMarket(0, 0, Side::Buy, 1)};

  // Разбор до LastStep = 2: бары шага 3 не влияют ни на расхождения, ни на
  // независимый портфель (INV-1, UpToStep = 2).
  const DivergenceResult R1 = analyzeDivergences(
      makeSpec(Tl1, "10000"), Tl1, Human, 2, {{"a1", AlgoSrc, {}}});
  const DivergenceResult R2 = analyzeDivergences(
      makeSpec(Tl2, "10000"), Tl2, Human, 2, {{"a1", AlgoSrc, {}}});

  EXPECT_EQ(R1.Divergences.size(), R2.Divergences.size());
  ASSERT_EQ(R1.Independent.size(), 1u);
  ASSERT_EQ(R2.Independent.size(), 1u);
  ASSERT_TRUE(R1.Independent[0].Run.Engine.has_value());
  EXPECT_EQ(R1.Independent[0].Run.Engine->currentStep(), 2);
  EXPECT_EQ(R1.Independent[0].Digest, R2.Independent[0].Digest);
  EXPECT_EQ(R1.Independent[0].PortfolioReport.FinalEquity,
            R2.Independent[0].PortfolioReport.FinalEquity);

  // Независимый портфель с Benchmark и Ruonia на многодневной шкале +
  // проверка остановки на LastStep (шаг 1 из 3).
  const auto MultiTl = makeTimeline({
      SimpleBar{"100", "101", "99", "100", 10000, 0},
      SimpleBar{"100", "101", "99", "105", 10000, 1},
      SimpleBar{"105", "110", "104", "110", 10000, 2},
  });
  BenchmarkSeries Bench;
  Bench.FirstOpen = Decimal::parse("1000");
  Bench.Closes = {
      DailyClose{20240201, Decimal::parse("1010")},
      DailyClose{20240202, Decimal::parse("1050")},
      DailyClose{20240205, Decimal::parse("1200")},
  };
  const RuoniaSeries Ruonia = makeRuonia();
  const DivergenceResult BenchRes =
      analyzeDivergences(makeSpec(MultiTl, "10000"), MultiTl, {}, 1,
                         {{"a1", AlgoSrc, {}}}, Bench, Ruonia, 20240131);
  ASSERT_EQ(BenchRes.Independent.size(), 1u);
  EXPECT_TRUE(
      BenchRes.Independent[0].PortfolioReport.Stats.Sharpe.has_value() ||
      BenchRes.Independent[0].PortfolioReport.Stats.TradingDays == 2);
  EXPECT_TRUE(BenchRes.Independent[0]
                  .PortfolioReport.Stats.BenchmarkReturn.has_value());

  // Stop / TakeProfit / автоснятие Sell при обнулении позиции человека не
  // генерируют расхождений (защитные типы исключаются из сравнения).
  const auto TlStop = makeTimeline({
      SimpleBar{"100", "105", "95", "100"},
      SimpleBar{"100", "105", "96", "100"},
      SimpleBar{"100", "105", "95", "100"},
      SimpleBar{"100", "105", "95", "100"},
  });
  const std::vector<JournalEntry> HumanTrade{
      placeMarket(0, 0, Side::Buy, 1),
      placeMarket(1, 1, Side::Sell, 1),
  };
  const std::string ProtectiveSrc = R"dsl(
rule "stop"
  when position > 0
  then sell all stop entry_price * 0.9
  because "стоп"

rule "tp"
  when position > 0
  then sell all take_profit entry_price * 1.1
  because "тейк"
)dsl";
  const DivergenceResult ProtRes =
      analyzeDivergences(makeSpec(TlStop, "10000"), TlStop, HumanTrade, 3,
                         {{"prot", ProtectiveSrc, {}}});
  // Только два Excess от человека (buy и sell), ни одного Missed от Stop/TP.
  ASSERT_EQ(ProtRes.Divergences.size(), 2u);
  EXPECT_EQ(ProtRes.Divergences[0].Type, DivergenceType::Excess);
  EXPECT_EQ(ProtRes.Divergences[1].Type, DivergenceType::Excess);
}

RC_GTEST_PROP(DivergenceProperty, Inv4DeterminismAndInv1FutureIsolation, ()) {
  const auto BaseTl = makeTimeline({
      SimpleBar{"100", "105", "95", "102"},
      SimpleBar{"102", "108", "99", "104"},
      SimpleBar{"104", "106", "97", "101"},
      SimpleBar{"101", "110", "100", "108"},
      SimpleBar{"108", "112", "105", "110"},
  });
  const auto MutatedTl = makeTimeline({
      SimpleBar{"100", "105", "95", "102"},
      SimpleBar{"102", "108", "99", "104"},
      SimpleBar{"104", "106", "97", "101"},
      SimpleBar{"500", "550", "450", "520"},
      SimpleBar{"520", "560", "480", "540"},
  });
  const std::int64_t LastStep = 2;
  const int Count = *rc::gen::inRange(0, 4);
  std::vector<JournalEntry> Human;
  std::int64_t PrevStep = 0;
  for (int I = 0; I < Count; ++I) {
    PrevStep = *rc::gen::inRange<std::int64_t>(PrevStep, LastStep + 1);
    const Side S = *rc::gen::element(Side::Buy, Side::Sell);
    const std::int64_t Lots = *rc::gen::inRange<std::int64_t>(1, 4);
    Human.push_back(placeMarket(I, PrevStep, S, Lots));
  }
  const std::string AlgoSrc = R"dsl(
rule "r"
  when close > open
  then buy 1 lots market
  because "рост"
)dsl";
  const DivergenceResult A =
      analyzeDivergences(makeSpec(BaseTl, "10000"), BaseTl, Human, LastStep,
                         {{"a1", AlgoSrc, {}}});
  const DivergenceResult B =
      analyzeDivergences(makeSpec(BaseTl, "10000"), BaseTl, Human, LastStep,
                         {{"a1", AlgoSrc, {}}});
  const DivergenceResult C =
      analyzeDivergences(makeSpec(MutatedTl, "10000"), MutatedTl, Human,
                         LastStep, {{"a1", AlgoSrc, {}}});

  RC_ASSERT(A.Divergences.size() == B.Divergences.size());
  RC_ASSERT(A.Divergences.size() == C.Divergences.size());
  for (std::size_t I = 0; I < A.Divergences.size(); ++I) {
    RC_ASSERT(A.Divergences[I].Seq == B.Divergences[I].Seq);
    RC_ASSERT(A.Divergences[I].Cost == B.Divergences[I].Cost);
    RC_ASSERT(A.Divergences[I].CostPercent == B.Divergences[I].CostPercent);
    RC_ASSERT(A.Divergences[I].Cost == C.Divergences[I].Cost);
    RC_ASSERT(A.Divergences[I].CostPercent == C.Divergences[I].CostPercent);
  }
  RC_ASSERT(A.Independent[0].Digest == B.Independent[0].Digest);
  RC_ASSERT(A.Independent[0].Digest == C.Independent[0].Digest);
}

TEST(Divergence, PositionReopenedOnBarCancelsVirtualSellOrders) {
  // Шаг 0: человек buy 1 market (исполняется на шаге 1 @ 100 -> position=1).
  // Шаг 1: алгоритм ставит sell 1 limit 150; человек на шаге 1 подаёт
  // sell 1 market (исполняется на шаге 2 Open=100 -> pos=0) и buy 1 limit 95
  // (исполняется на шаге 2 Low=94 -> pos=1, PositionReopenedOnBar=true).
  // На шаге 2 старая виртуальная sell-заявка должна сняться, и алгоритм
  // создаёт новое действие sell 1 limit 150 (Missed на шаге 2).
  const auto Tl = makeTimeline({
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "94", "100"},
      SimpleBar{"100", "101", "99", "100"},
  });
  const SessionSpec Spec = makeSpec(Tl, "100000");
  const std::vector<JournalEntry> Human{
      placeMarket(0, 0, Side::Buy, 1),
      placeMarket(1, 1, Side::Sell, 1),
      placeLimit(2, 1, Side::Buy, 1, "95"),
  };
  const std::string SellSrc = R"dsl(
rule "sell_high"
  when position > 0
  then sell 1 lots limit 150
  because "продажа"
)dsl";
  const DivergenceResult Res =
      analyzeDivergences(Spec, Tl, Human, 3, {{"a1", SellSrc, {}}});
  int MissedSells = 0;
  std::vector<std::int64_t> MissedSteps;
  for (const Divergence &D : Res.Divergences) {
    if (D.Type == DivergenceType::Missed && D.AlgoLots == -1) {
      ++MissedSells;
      MissedSteps.push_back(D.AlgoStep);
    }
  }
  EXPECT_EQ(MissedSells, 2);
  EXPECT_EQ(MissedSteps, (std::vector<std::int64_t>{1, 2}));
}

TEST(Divergence, VirtualOrdersShareLiquidityBudgetInPhaseOrder) {
  // LiquidityShare = 0.1, VolumeLots = 10 -> единый Cap = 1 на баре 1.
  // На шаге 0 алгоритм ставит buy 1 market и buy 1 limit 95.
  // На шаге 1 (Open=94): Market забирает весь Cap=1, Limit остаётся активной
  // и дедуплицирует повторное правило limit 95 на шаге 1.
  const auto Tl = makeTimeline({
      SimpleBar{"100", "101", "99", "100", 10},
      SimpleBar{"94", "102", "93", "101", 10},
      SimpleBar{"101", "102", "100", "101", 10},
  });
  CostConfig Costs = CostConfig::zero();
  Costs.LiquidityShare = Decimal::parse("0.1");
  const SessionSpec Spec = makeSpec(Tl, "100000", Costs);
  const std::string Src = R"dsl(
rule "mkt"
  when close == 100
  then buy 1 lots market
  because "рынок"

rule "lim"
  when close == 100 or close == 101
  then buy 1 lots limit 95
  because "лимит"
)dsl";
  const DivergenceResult Res =
      analyzeDivergences(Spec, Tl, {}, 2, {{"a1", Src, {}}});
  ASSERT_EQ(Res.Divergences.size(), 1u);
  EXPECT_EQ(Res.Divergences[0].AlgoStep, 0);
  EXPECT_EQ(Res.Divergences[0].AlgoLots, 2);

  // Stop -> Limit также делят единый Cap=1 на баре 1.
  const auto StopTl = makeTimeline({
      SimpleBar{"100", "101", "99", "100", 10},
      SimpleBar{"100", "103", "98", "101", 10},
      SimpleBar{"101", "102", "100", "101", 10},
  });
  const std::string StopSrc = R"dsl(
rule "stp"
  when close == 100
  then buy 1 lots stop 102
  because "стоп"

rule "lim2"
  when close == 100 or close == 101
  then buy 1 lots limit 99
  because "лимит"
)dsl";
  const DivergenceResult StopRes = analyzeDivergences(
      makeSpec(StopTl, "100000", Costs), StopTl, {}, 2, {{"a1", StopSrc, {}}});
  ASSERT_EQ(StopRes.Divergences.size(), 1u);
  EXPECT_EQ(StopRes.Divergences[0].AlgoStep, 0);
  EXPECT_EQ(StopRes.Divergences[0].AlgoLots, 1);
}

TEST(Divergence, MissedProfitableTradeWithOpposingUnfilledSellLimit) {
  // Шаг 0: человек открывает 3 лота (исполнение на шаге 1 @ 100), чтобы на
  // шаге 4 (вне окна W=3 от шага 0) иметь позицию >= 3 для встречной Sell
  // Limit 150.
  const auto Tl = makeTimeline({
      SimpleBar{"100", "101", "99", "99"},
      SimpleBar{"100", "101", "99", "101"},
      SimpleBar{"101", "102", "100", "101"},
      SimpleBar{"101", "102", "100", "101"},
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "112", "99", "110"},
      SimpleBar{"120", "121", "119", "120"},
      SimpleBar{"120", "121", "119", "120"},
  });
  const SessionSpec Spec = makeSpec(Tl, "100000");
  const std::string NoOpSrc =
      "rule \"no\" when false then buy 1 lots market because \"no\"\n";
  const std::string BuyAt4Src =
      "rule \"b\" when close == 100 then buy 2 lots market because \"b\"\n";
  const std::string SellLimAt4Src =
      "rule \"s\" when close == 100 then sell 1 lots limit 150 because "
      "\"s\"\n";

  // 1) На шаге 4: Buy 2 Market + Sell 2 Limit 150 (не исполнена) => Act.Lots=0.
  // Сделка входа шага 4 (EntryStep=5, Lots=2, NetPnl=40) не теряется из Missed,
  // а a2 (купил на шаге 4) не попадает в AlgorithmIds.
  const std::vector<JournalEntry> NetZeroHuman{
      placeMarket(0, 0, Side::Buy, 3),
      placeMarket(1, 4, Side::Buy, 2),
      placeLimit(2, 4, Side::Sell, 2, "150"),
      placeMarket(3, 5, Side::Sell, 5),
  };
  const DivergenceResult ZeroRes = analyzeDivergences(
      Spec, Tl, NetZeroHuman, 7, {{"a1", NoOpSrc, {}}, {"a2", BuyAt4Src, {}}});
  ASSERT_EQ(ZeroRes.Missed.size(), 2u);
  const MissedProfitableTrade &Trade4 = ZeroRes.Missed[1];
  EXPECT_EQ(Trade4.EntryStep, 5);
  EXPECT_EQ(Trade4.ExitStep, 6);
  EXPECT_EQ(Trade4.Lots, 2);
  EXPECT_EQ(Trade4.NetPnl, Decimal::parse("40"));
  EXPECT_EQ(Trade4.AlgorithmIds, (std::vector<std::string>{"a1"}));
  EXPECT_TRUE(Trade4.DivergenceSeqs.empty());

  // 2) На шаге 4: Buy 2 Market + Sell 3 Limit 150 (не исполнена) => Lots=-1.
  // Совпадение a2 по Sell не скрывает пропущенный вход, а Sell Excess у a1
  // не попадает в DivergenceSeqs.
  const std::vector<JournalEntry> NetNegHuman{
      placeMarket(0, 0, Side::Buy, 3),
      placeMarket(1, 4, Side::Buy, 2),
      placeLimit(2, 4, Side::Sell, 3, "150"),
      placeMarket(3, 5, Side::Sell, 5),
  };
  const DivergenceResult NegRes = analyzeDivergences(
      Spec, Tl, NetNegHuman, 7,
      {{"a1", NoOpSrc, {}}, {"a2", SellLimAt4Src, {}}, {"a3", BuyAt4Src, {}}});
  ASSERT_EQ(NegRes.Missed.size(), 2u);
  const MissedProfitableTrade &NegTrade4 = NegRes.Missed[1];
  EXPECT_EQ(NegTrade4.EntryStep, 5);
  EXPECT_EQ(NegTrade4.AlgorithmIds, (std::vector<std::string>{"a1", "a2"}));
  EXPECT_TRUE(NegTrade4.DivergenceSeqs.empty());
}

TEST(Divergence, CooperativeCancellationStopsEarly) {
  const auto Tl = makeTimeline({
      SimpleBar{"100", "105", "95", "100"},
      SimpleBar{"100", "105", "95", "100"},
      SimpleBar{"100", "105", "95", "100"},
      SimpleBar{"100", "105", "95", "100"},
  });
  const SessionSpec Spec = makeSpec(Tl, "100000");
  const std::string AlgoSrc =
      "rule \"b\" when position == 0 then buy 1 lots market because \"b\"\n";

  int Checks = 0;
  const DivergenceResult EarlyRes = analyzeDivergences(
      Spec, Tl, {}, 3, {{"a1", AlgoSrc, {}}}, std::nullopt, std::nullopt, 0,
      [&]() { return ++Checks >= 2; });
  EXPECT_TRUE(EarlyRes.Cancelled);
  EXPECT_TRUE(EarlyRes.Independent.empty());

  // Отмена на фазе assembleIndependent (внутри runStrategy).
  int LateChecks = 0;
  const DivergenceResult IndepCancelRes = analyzeDivergences(
      Spec, Tl, {}, 3, {{"a1", AlgoSrc, {}}}, std::nullopt, std::nullopt, 0,
      [&]() { return ++LateChecks >= 20; });
  EXPECT_TRUE(IndepCancelRes.Cancelled);
}

} // namespace
} // namespace core
