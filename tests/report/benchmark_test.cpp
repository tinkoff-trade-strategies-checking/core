#include "core/report/benchmark.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "core/report/metrics.hpp"
#include "support/fixture1.hpp"

// Кривая бенчмарка отчёта и счётчик «стоп и тейк одной свечой».

namespace core {
namespace {

Decimal d(const char *Text) { return Decimal::parse(Text); }

// fixture-1 по D1: 10 шагов, по шагу на день 2024-01-09…22.
Timeline fixtureTimeline() {
  auto F = testing::fixture1();
  return Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                        Timeframe::D1, 20240109, 20240122);
}

// Индекс 1000 на открытии первого дня; close - 1010, 1020, … по дням, кроме
// 2024-01-11 (у индекса нет бара).
BenchmarkSeries series() {
  BenchmarkSeries S;
  S.FirstOpen = d("1000");
  const std::int32_t Days[] = {20240109, 20240110, 20240112, 20240115, 20240116,
                               20240117, 20240118, 20240119, 20240122};
  int I = 1;
  for (const std::int32_t Day : Days)
    S.Closes.push_back(DailyClose{Day, Decimal::fromUnits(1000 + 10 * I++)});
  return S;
}

TEST(Benchmark, RangeIsFirstOpenAndLastClose) {
  const auto Range = benchmarkRange(series());
  ASSERT_TRUE(Range);
  EXPECT_EQ(Range->first, d("1000"));
  EXPECT_EQ(Range->second, d("1090"));
  EXPECT_FALSE(benchmarkRange(BenchmarkSeries{d("1000"), {}}));
}

TEST(Benchmark, EquityPerDayInCapitalRubles) {
  const Timeline T = fixtureTimeline();
  const std::vector<EquityPoint> Curve =
      benchmarkEquity(series(), d("1000000"), T, 9);
  // День без бара индекса (шаг 2) пропущен.
  ASSERT_EQ(Curve.size(), 9u);
  EXPECT_EQ(Curve[0].Step, 0);
  EXPECT_EQ(Curve[0].Equity, d("1010000"));
  EXPECT_EQ(Curve[1].Step, 1);
  EXPECT_EQ(Curve[2].Step, 3);
  EXPECT_EQ(Curve[2].Equity, d("1030000"));
  EXPECT_EQ(Curve.back().Step, 9);
  EXPECT_EQ(Curve.back().Equity, d("1090000"));
}

TEST(Benchmark, StopsAtLastStepAndRounds) {
  const Timeline T = fixtureTimeline();
  BenchmarkSeries S = series();
  S.FirstOpen = d("3");
  const std::vector<EquityPoint> Curve = benchmarkEquity(S, d("1"), T, 1);
  ASSERT_EQ(Curve.size(), 2u);                    // шаги 0 и 1
  EXPECT_EQ(Curve[0].Equity, d("336.666666667")); // 1010 / 3, 9 знаков HALF_UP
}

TEST(Benchmark, BadInput) {
  const Timeline T = fixtureTimeline();
  EXPECT_THROW((void)benchmarkEquity(series(), d("1"), T, 10),
               std::invalid_argument);
  EXPECT_THROW((void)benchmarkEquity(series(), d("1"), T, -1),
               std::invalid_argument);
  EXPECT_THROW((void)benchmarkEquity(BenchmarkSeries{d("0"), {}}, d("1"), T, 0),
               std::invalid_argument);
}

TEST(Benchmark, ReportCarriesCurveAsIs) {
  MetricsInput In;
  In.InitialCash = d("1000000");
  In.Equity = {{0, d("1000000")}};
  In.TradingDayOfStep = {1};
  In.BenchmarkEquity = {{0, d("1010000")}};
  const Report Report = computeReport(In);
  ASSERT_EQ(Report.BenchmarkEquity.size(), 1u);
  EXPECT_EQ(Report.BenchmarkEquity[0].Equity, d("1010000"));
}

TEST(Metrics, CountsStopAndTakeOnSameBar) {
  MetricsInput In;
  In.InitialCash = d("1000000");
  In.Equity = {{0, d("1000000")}, {1, d("1000000")}};
  In.TradingDayOfStep = {1, 2};
  In.RuoniaRates = {Decimal::zero(), Decimal::zero()};
  In.CalendarDays = {1, 1};
  Fill Plain;
  Fill Pessimistic;
  Pessimistic.StopAndTakeSameBar = true;
  In.Fills = {Plain, Pessimistic, Pessimistic};
  EXPECT_EQ(computeReport(In).Stats.StopTakeSameBar, 2);
}

} // namespace
} // namespace core
