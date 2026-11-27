#include "core/report/simple_benchmarks.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "core/exec/costs.hpp"
#include "core/market/bar_source.hpp"
#include "support/fixture1.hpp"

// Бенчмарки «Купил и держал» и «Надёжный вклад» (core#81).

namespace core {
namespace {

Decimal d(const char *Text) { return Decimal::parse(Text); }

// fixture-1 по D1: 10 шагов (по шагу на день 2024-01-09…22). Бумага A - лот 10,
// close 100…108; бумага B - лот 10000, цена 0.02, бара на шаге 3 нет.
Timeline timelineOf(const std::vector<std::string> &Keys) {
  auto F = testing::fixture1();
  std::vector<Instrument> Papers;
  for (const Instrument &I : F.Instruments) {
    for (const std::string &Key : Keys) {
      if (I.Key == Key)
        Papers.push_back(I);
    }
  }
  return Timeline::load(F.Bars, F.Calendar, F.SnapshotId, Papers, Timeframe::D1,
                        20240109, 20240122);
}

RuoniaSeries flatRate() {
  return RuoniaSeries{{DailyRate{20240108, d("15.0")}}};
}

TEST(BuyHold, SinglePaperExactCurveWithZeroCosts) {
  const Timeline T = timelineOf({"A"});
  const auto Curve = buyHoldEquity(T, 9, d("1000000"), CostConfig::zero());
  ASSERT_EQ(Curve.size(), 10u);
  // 1 000 000 / (100 * 10) = 1000 лотов, остаток денег 0.
  EXPECT_EQ(Curve.front().Step, 0);
  EXPECT_EQ(Curve.front().Equity, d("1000000")); // close шага 0 = 100
  EXPECT_EQ(Curve[2].Equity, d("1030000"));      // close 103
  EXPECT_EQ(Curve.back().Step, 9);
  EXPECT_EQ(Curve.back().Equity, d("1080000"));
  EXPECT_EQ(returnPct(Curve, d("1000000")), d("8"));
}

TEST(BuyHold, TwoPapersEqualWeightAndMissingBar) {
  const Timeline T = timelineOf({"A", "B"});
  const auto Curve = buyHoldEquity(T, 9, d("1000000"), CostConfig::zero());
  ASSERT_EQ(Curve.size(), 10u);
  // По 500 000: A - 500 лотов по 1000, B - 2500 лотов по 200.
  EXPECT_EQ(Curve.front().Equity, d("1000000"));
  // Шаг 3: у B нет бара, берётся последний close (0.02); A close 106.
  EXPECT_EQ(Curve[3].Equity, d("1030000"));
  EXPECT_EQ(Curve.back().Equity, d("1040000")); // 540 000 + 500 000
  EXPECT_EQ(returnPct(Curve, d("1000000")), d("4"));
}

TEST(BuyHold, ShortPeriodIsOnlyFirstSteps) {
  const Timeline T = timelineOf({"A"});
  const auto Curve = buyHoldEquity(T, 1, d("1000000"), CostConfig::zero());
  ASSERT_EQ(Curve.size(), 2u);
  EXPECT_EQ(Curve.back().Equity, d("1010000"));
  EXPECT_EQ(returnPct(Curve, d("1000000")), d("1"));
}

TEST(BuyHold, DefaultCostsReduceLotsAndEquity) {
  const Timeline T = timelineOf({"A"});
  const Decimal Cash = d("1000000");
  const auto Free = buyHoldEquity(T, 9, Cash, CostConfig::zero());
  const auto Paid = buyHoldEquity(T, 9, Cash, CostConfig::defaults());
  ASSERT_EQ(Paid.size(), Free.size());
  // Издержки входа (комиссия, сбор, спред, проскальзывание) всегда снижают
  // результат, но не больше нескольких процентов.
  EXPECT_LT(Paid.front().Equity, Cash);
  EXPECT_GT(Paid.front().Equity, d("980000"));
  EXPECT_LT(Paid.back().Equity, Free.back().Equity);
  // Деньги не уходят в минус: бюджет не превышен.
  EXPECT_GT(Paid.front().Equity, Decimal::zero());
}

TEST(BuyHold, TooLittleCashBuysNothingAndKeepsCash) {
  const Timeline T = timelineOf({"A"});
  // Лот стоит 1000: на 500 купить нечего, капитал остаётся деньгами.
  const auto Curve = buyHoldEquity(T, 9, d("500"), CostConfig::zero());
  ASSERT_EQ(Curve.size(), 10u);
  for (const EquityPoint &P : Curve)
    EXPECT_EQ(P.Equity, d("500"));
  EXPECT_EQ(returnPct(Curve, d("500")), d("0"));
}

TEST(BuyHold, DeterministicAndRejectsBadInput) {
  const Timeline T = timelineOf({"A", "B"});
  const auto First = buyHoldEquity(T, 9, d("1000000"), CostConfig::defaults());
  const auto Second = buyHoldEquity(T, 9, d("1000000"), CostConfig::defaults());
  ASSERT_EQ(First.size(), Second.size());
  for (std::size_t I = 0; I < First.size(); ++I) {
    EXPECT_EQ(First[I].Step, Second[I].Step);
    EXPECT_EQ(First[I].Equity, Second[I].Equity);
  }
  EXPECT_THROW((void)buyHoldEquity(T, 9, d("0"), CostConfig::zero()),
               std::invalid_argument);
  EXPECT_THROW((void)buyHoldEquity(T, 10, d("1000"), CostConfig::zero()),
               std::invalid_argument);
  EXPECT_THROW((void)buyHoldEquity(T, -1, d("1000"), CostConfig::zero()),
               std::invalid_argument);
}

TEST(RuoniaBenchmark, CompoundsActual365WithCalendarGaps) {
  const Timeline T = timelineOf({"A"});
  const auto Curve = ruoniaEquity(flatRate(), T, 9, d("1000000"), 0);
  ASSERT_EQ(Curve.size(), 10u);
  // Эталон: E = E + E * 15 * Delta / 36500 (9 знаков HALF_UP на шаге);
  // Delta = 3 на шагах 4 (пятница -> понедельник) и 9.
  EXPECT_EQ(Curve[0].Equity, d("1000410.96"));
  EXPECT_EQ(Curve[1].Equity, d("1000822.09"));
  EXPECT_EQ(Curve[2].Equity, d("1001233.38"));
  EXPECT_EQ(Curve[3].Equity, d("1001644.85"));
  EXPECT_EQ(Curve[4].Equity, d("1002879.75"));
  EXPECT_EQ(Curve[5].Equity, d("1003291.90"));
  EXPECT_EQ(Curve[9].Equity, d("1005767.80"));
  EXPECT_EQ(returnPct(Curve, d("1000000")), d("0.58"));
}

TEST(RuoniaBenchmark, ShortPeriodAndMissingRates) {
  const Timeline T = timelineOf({"A"});
  const auto One = ruoniaEquity(flatRate(), T, 0, d("1000000"), 0);
  ASSERT_EQ(One.size(), 1u);
  EXPECT_EQ(One[0].Equity, d("1000410.96"));
  // Ставок до первого дня нет совсем.
  const RuoniaSeries Late{{DailyRate{20240122, d("15.0")}}};
  EXPECT_THROW((void)ruoniaEquity(Late, T, 9, d("1000000"), 0),
               MarketDataError);
  EXPECT_THROW((void)ruoniaEquity(flatRate(), T, 9, d("-1"), 0),
               std::invalid_argument);
}

TEST(RuoniaBenchmark, Deterministic) {
  const Timeline T = timelineOf({"A", "B"});
  EXPECT_EQ(
      ruoniaEquity(flatRate(), T, 9, d("1000000"), 20240108).back().Equity,
      ruoniaEquity(flatRate(), T, 9, d("1000000"), 20240108).back().Equity);
}

TEST(ReturnPct, NegativeEmptyAndRounding) {
  EXPECT_FALSE(returnPct({}, d("1000")).has_value());
  EXPECT_EQ(returnPct({EquityPoint{0, d("900")}}, d("1000")), d("-10"));
  // 1 / 3 % -> 0.33; 2 / 3 % -> 0.67 (HALF_UP).
  EXPECT_EQ(returnPct({EquityPoint{0, d("301000")}}, d("300000")), d("0.33"));
  EXPECT_EQ(returnPct({EquityPoint{0, d("302000")}}, d("300000")), d("0.67"));
}

TEST(SimpleBenchmarks, BuildAndApplyToReport) {
  const Timeline T = timelineOf({"A"});
  const SimpleBenchmarks B = buildSimpleBenchmarks(
      T, 9, d("1000000"), CostConfig::zero(), flatRate(), 0);
  EXPECT_EQ(B.BuyHoldReturnPct, d("8"));
  EXPECT_EQ(B.RuoniaReturnPct, d("0.58"));
  EXPECT_EQ(B, buildSimpleBenchmarks(T, 9, d("1000000"), CostConfig::zero(),
                                     flatRate(), 0));

  Report R;
  EXPECT_TRUE(R.BuyHoldEquity.empty());
  EXPECT_FALSE(R.BuyHoldReturnPct.has_value());
  applySimpleBenchmarks(R, B);
  EXPECT_EQ(R.BuyHoldEquity.size(), 10u);
  EXPECT_EQ(R.RuoniaEquity.size(), 10u);
  EXPECT_EQ(R.BuyHoldReturnPct, d("8"));
  EXPECT_EQ(R.RuoniaReturnPct, d("0.58"));
}

TEST(SimpleBenchmarks, MissingRuoniaDoesNotBreakReport) {
  const Timeline T = timelineOf({"A"});
  const SimpleBenchmarks NoSeries = buildSimpleBenchmarks(
      T, 9, d("1000000"), CostConfig::zero(), std::nullopt, 0);
  EXPECT_FALSE(NoSeries.BuyHoldEquity.empty());
  EXPECT_TRUE(NoSeries.RuoniaEquity.empty());
  EXPECT_FALSE(NoSeries.RuoniaReturnPct.has_value());

  const SimpleBenchmarks EmptySeries = buildSimpleBenchmarks(
      T, 9, d("1000000"), CostConfig::zero(), RuoniaSeries{}, 0);
  EXPECT_TRUE(EmptySeries.RuoniaEquity.empty());

  const SimpleBenchmarks Late =
      buildSimpleBenchmarks(T, 9, d("1000000"), CostConfig::zero(),
                            RuoniaSeries{{DailyRate{20240122, d("15.0")}}}, 0);
  EXPECT_TRUE(Late.RuoniaEquity.empty());
  EXPECT_EQ(Late.BuyHoldReturnPct, d("8"));
}

} // namespace
} // namespace core
