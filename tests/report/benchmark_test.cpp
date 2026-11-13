#include "core/report/benchmark.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "core/report/metrics.hpp"
#include "support/fixture1.hpp"

// Кривая бенчмарка отчёта и счётчик «стоп и тейк одной свечой».

namespace core {
namespace {

Decimal D(const char* text) { return Decimal::parse(text); }

// fixture-1 по D1: 10 шагов, по шагу на день 2024-01-09…22.
Timeline fixtureTimeline() {
  auto f = testing::fixture1();
  return Timeline::load(f.bars, f.calendar, f.snapshotId, f.instruments, Timeframe::D1, 20240109, 20240122);
}

// Индекс 1000 на открытии первого дня; close - 1010, 1020, … по дням, кроме 2024-01-11 (у индекса нет бара).
BenchmarkSeries series() {
  BenchmarkSeries s;
  s.firstOpen = D("1000");
  const std::int32_t days[] = {20240109, 20240110, 20240112, 20240115, 20240116,
                               20240117, 20240118, 20240119, 20240122};
  int i = 1;
  for (const std::int32_t day : days) s.closes.push_back(DailyClose{day, Decimal::fromUnits(1000 + 10 * i++)});
  return s;
}

TEST(Benchmark, RangeIsFirstOpenAndLastClose) {
  const auto range = benchmarkRange(series());
  ASSERT_TRUE(range);
  EXPECT_EQ(range->first, D("1000"));
  EXPECT_EQ(range->second, D("1090"));
  EXPECT_FALSE(benchmarkRange(BenchmarkSeries{D("1000"), {}}));
}

TEST(Benchmark, EquityPerDayInCapitalRubles) {
  const Timeline t = fixtureTimeline();
  const std::vector<EquityPoint> curve = benchmarkEquity(series(), D("1000000"), t, 9);
  // День без бара индекса (шаг 2) пропущен.
  ASSERT_EQ(curve.size(), 9u);
  EXPECT_EQ(curve[0].step, 0);
  EXPECT_EQ(curve[0].equity, D("1010000"));
  EXPECT_EQ(curve[1].step, 1);
  EXPECT_EQ(curve[2].step, 3);
  EXPECT_EQ(curve[2].equity, D("1030000"));
  EXPECT_EQ(curve.back().step, 9);
  EXPECT_EQ(curve.back().equity, D("1090000"));
}

TEST(Benchmark, StopsAtLastStepAndRounds) {
  const Timeline t = fixtureTimeline();
  BenchmarkSeries s = series();
  s.firstOpen = D("3");
  const std::vector<EquityPoint> curve = benchmarkEquity(s, D("1"), t, 1);
  ASSERT_EQ(curve.size(), 2u);  // шаги 0 и 1
  EXPECT_EQ(curve[0].equity, D("336.666666667"));  // 1010 / 3, 9 знаков HALF_UP
}

TEST(Benchmark, BadInput) {
  const Timeline t = fixtureTimeline();
  EXPECT_THROW((void)benchmarkEquity(series(), D("1"), t, 10), std::invalid_argument);
  EXPECT_THROW((void)benchmarkEquity(series(), D("1"), t, -1), std::invalid_argument);
  EXPECT_THROW((void)benchmarkEquity(BenchmarkSeries{D("0"), {}}, D("1"), t, 0), std::invalid_argument);
}

TEST(Benchmark, ReportCarriesCurveAsIs) {
  MetricsInput in;
  in.initialCash = D("1000000");
  in.equity = {{0, D("1000000")}};
  in.tradingDayOfStep = {1};
  in.benchmarkEquity = {{0, D("1010000")}};
  const Report report = computeReport(in);
  ASSERT_EQ(report.benchmarkEquity.size(), 1u);
  EXPECT_EQ(report.benchmarkEquity[0].equity, D("1010000"));
}

TEST(Metrics, CountsStopAndTakeOnSameBar) {
  MetricsInput in;
  in.initialCash = D("1000000");
  in.equity = {{0, D("1000000")}, {1, D("1000000")}};
  in.tradingDayOfStep = {1, 2};
  Fill plain;
  Fill pessimistic;
  pessimistic.stopAndTakeSameBar = true;
  in.fills = {plain, pessimistic, pessimistic};
  EXPECT_EQ(computeReport(in).metrics.stopTakeSameBar, 2);
}

}  // namespace
}  // namespace core
