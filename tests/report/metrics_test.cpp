#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/money/decimal.hpp"
#include "core/report/metrics.hpp"

using core::Decimal;
using core::Fill;
using core::MetricsInput;
using core::RoundTrip;

namespace {

constexpr double kTol = 1e-9;

Decimal D(const char* text) { return Decimal::parse(text); }

// Дневной ряд: по шагу на день, шаг i - день i.
MetricsInput dailyInput(const std::vector<const char*>& equity, const char* initialCash = "1000000") {
  MetricsInput in;
  in.initialCash = D(initialCash);
  for (std::size_t i = 0; i < equity.size(); ++i) {
    in.equity.push_back({static_cast<std::int64_t>(i), D(equity[i])});
    in.tradingDayOfStep.push_back(static_cast<std::int32_t>(i));
  }
  return in;
}

MetricsInput fiveDays() { return dailyInput({"1010000", "1005000", "1020000", "990000", "1030000"}); }

Fill fill(const char* value, const char* commission, const char* fee, const char* spread = "0",
          const char* slippage = "0") {
  Fill f;
  f.value = D(value);
  f.commission = D(commission);
  f.fee = D(fee);
  f.spreadCost = D(spread);
  f.slippageCost = D(slippage);
  return f;
}

RoundTrip trip(const char* netPnl) {
  RoundTrip t{};
  t.netPnl = D(netPnl);
  return t;
}

TEST(Metrics, DailySeries) {
  const core::Report r = core::computeReport(fiveDays());
  const core::Metrics& m = r.metrics;
  EXPECT_NEAR(m.totalReturn, 0.03, kTol);
  ASSERT_TRUE(m.cagr);
  EXPECT_NEAR(*m.cagr, 3.4360468599, kTol);
  ASSERT_TRUE(m.volatility);
  EXPECT_NEAR(*m.volatility, 0.4088278812, kTol);
  ASSERT_TRUE(m.sharpe);
  EXPECT_NEAR(*m.sharpe, 3.8176079043, kTol);
  ASSERT_TRUE(m.sortino);
  EXPECT_NEAR(*m.sortino, 7.3710457334, kTol);
  EXPECT_NEAR(m.maxDrawdown, 0.0294117647, kTol);
  EXPECT_EQ(m.maxDrawdownSteps, 1);
  EXPECT_EQ(m.tradingDays, 5);
  EXPECT_EQ(r.finalEquity, D("1030000"));
  EXPECT_EQ(r.equity.size(), 5u);
}

TEST(Metrics, OneDayHasNoVolatility) {
  const core::Metrics m = core::computeReport(dailyInput({"1010000"})).metrics;
  EXPECT_EQ(m.tradingDays, 1);
  EXPECT_FALSE(m.volatility);
  EXPECT_FALSE(m.sharpe);
  EXPECT_FALSE(m.sortino);
  ASSERT_TRUE(m.cagr);
  EXPECT_NEAR(*m.cagr, std::pow(1.01, 252.0) - 1.0, 1e-6);
}

TEST(Metrics, OneLosingDayHasNoSortino) {
  EXPECT_FALSE(core::computeReport(dailyInput({"990000"})).metrics.sortino);
}

TEST(Metrics, NoEquityPoints) {
  MetricsInput in;
  in.initialCash = D("1000000");
  const core::Report r = core::computeReport(in);
  EXPECT_EQ(r.finalEquity, D("1000000"));
  EXPECT_EQ(r.metrics.tradingDays, 0);
  EXPECT_FALSE(r.metrics.cagr);
  EXPECT_FALSE(r.metrics.volatility);
  EXPECT_EQ(r.metrics.totalReturn, 0.0);
  EXPECT_EQ(r.metrics.maxDrawdown, 0.0);
  EXPECT_EQ(r.metrics.turnover, 0.0);
}

TEST(Metrics, ConstantReturnHasNoSharpeAndSortino) {
  // Доходность каждый день одинаковая: разброса нет.
  const core::Metrics m = core::computeReport(dailyInput({"1100000", "1210000", "1331000"})).metrics;
  ASSERT_TRUE(m.volatility);
  EXPECT_NEAR(*m.volatility, 0.0, 1e-9);
  EXPECT_FALSE(m.sharpe);
  EXPECT_FALSE(m.sortino);  // нет убыточных дней
}

TEST(Metrics, TinySpreadHasNoSharpeAndSortino) {
  // Разброс порядка 1e-15 - шум округления double, не настоящий разброс: Sharpe и Sortino не определены.
  MetricsInput in = dailyInput({"1000000.000000001", "1000000.000000002", "1000000.000000003", "999999.999999999"});
  const core::Metrics m = core::computeReport(in).metrics;
  EXPECT_FALSE(m.sharpe);
  EXPECT_FALSE(m.sortino);
  ASSERT_TRUE(m.volatility);
  EXPECT_TRUE(std::isfinite(*m.volatility));
}

TEST(Metrics, BankruptHasFiniteMetrics) {
  MetricsInput in = dailyInput({"500000", "0"});
  in.fills = {fill("100000", "0", "0")};
  const core::Metrics m = core::computeReport(in).metrics;
  EXPECT_FALSE(m.cagr);  // итоговая стоимость 0
  EXPECT_NEAR(m.totalReturn, -1.0, kTol);
  EXPECT_NEAR(m.turnover, 100000.0 / 250000.0, kTol);
  EXPECT_TRUE(std::isfinite(m.maxDrawdown));

  in = dailyInput({"0", "0"});
  in.fills = {fill("100000", "0", "0")};
  const core::Metrics z = core::computeReport(in).metrics;
  EXPECT_EQ(z.turnover, 0.0);  // средняя стоимость 0
  EXPECT_FALSE(z.cagr);
  EXPECT_TRUE(std::isfinite(z.totalReturn));
}

TEST(Metrics, LastStepOfDayDefinesDailyReturn) {
  MetricsInput in;
  in.initialCash = D("1000000");
  in.tradingDayOfStep = {0, 0, 0, 1, 1, 1};
  const char* values[] = {"1000500", "1001000", "1002000", "1003000", "1000000", "1004000"};
  for (int i = 0; i < 6; ++i) in.equity.push_back({i, D(values[i])});
  const core::Metrics m = core::computeReport(in).metrics;
  EXPECT_EQ(m.tradingDays, 2);
  const double r1 = 1002000.0 / 1000000.0 - 1.0, r2 = 1004000.0 / 1002000.0 - 1.0;
  const double mu = (r1 + r2) / 2;
  const double sd = std::sqrt(((r1 - mu) * (r1 - mu) + (r2 - mu) * (r2 - mu)) / 1.0);
  ASSERT_TRUE(m.volatility);
  EXPECT_NEAR(*m.volatility, sd * std::sqrt(252.0), kTol);
  // Просадка считается по шагам, а не по дням: шаг 4 (1 000 000) ниже пика 1 003 000.
  EXPECT_NEAR(m.maxDrawdown, 1.0 - 1000000.0 / 1003000.0, kTol);
}

TEST(Metrics, DrawdownFromInitialCashAndRunLength) {
  // Пик - начальный капитал; ниже пика три шага подряд, затем новый максимум, затем один шаг ниже.
  const core::Metrics m =
      core::computeReport(dailyInput({"990000", "980000", "985000", "1010000", "1000000"})).metrics;
  EXPECT_NEAR(m.maxDrawdown, 0.02, kTol);
  EXPECT_EQ(m.maxDrawdownSteps, 3);
}

TEST(Metrics, ReturnToPeakEndsRun) {
  const core::Metrics m = core::computeReport(dailyInput({"990000", "1000000", "990000", "980000"})).metrics;
  EXPECT_EQ(m.maxDrawdownSteps, 2);
}

TEST(Metrics, CostsSummed) {
  MetricsInput in = fiveDays();
  // Сделки 1-3 таблицы эталонного портфеля (задача 16).
  in.fills = {fill("271540.00", "135.77", "27.16", "100.00", "5.00"),
              fill("134455.00", "67.23", "13.45", "50.00", "2.50"), fill("73575.00", "36.79", "7.36", "20.00", "0")};
  const core::Report r = core::computeReport(in);
  EXPECT_EQ(r.costs.commission, D("239.79"));
  EXPECT_EQ(r.costs.fee, D("47.97"));
  EXPECT_EQ(r.costs.spread, D("170.00"));
  EXPECT_EQ(r.costs.slippage, D("7.50"));
  EXPECT_EQ(r.metrics.fills, 3);
  // grossReturn = totalReturn + (комиссии + сборы) / E0.
  EXPECT_NEAR(r.metrics.grossReturn, 0.03 + (239.79 + 47.97) / 1000000.0, 1e-12);
}

TEST(Metrics, TurnoverIsTradedValueOverMeanDailyEquity) {
  MetricsInput in = fiveDays();
  in.fills = {fill("100000", "0", "0"), fill("102200", "0", "0")};
  // mean(E_d) = 1 011 000
  EXPECT_NEAR(core::computeReport(in).metrics.turnover, 202200.0 / 1011000.0, kTol);
}

TEST(Metrics, WinRateAndCounters) {
  MetricsInput in = fiveDays();
  EXPECT_FALSE(core::computeReport(in).metrics.winRate);
  in.roundTrips = {trip("100.50"), trip("-20"), trip("0"), trip("5")};
  in.rejections = 3;
  in.partialFills = 2;
  const core::Metrics m = core::computeReport(in).metrics;
  ASSERT_TRUE(m.winRate);
  EXPECT_NEAR(*m.winRate, 0.5, kTol);  // ноль - не выигрыш
  EXPECT_EQ(m.roundTrips, 4);
  EXPECT_EQ(m.rejections, 3);
  EXPECT_EQ(m.partialFills, 2);
}

TEST(Metrics, ConcentrationByPositionValues) {
  MetricsInput in = dailyInput({"2000", "2000", "2500"}, "2000");
  in.positionValuesByStep = {{D("600"), D("400")}, {D("0"), D("0")}, {D("1000"), D("0")}};
  const core::Metrics m = core::computeReport(in).metrics;
  // Шаг без позиций в среднем HHI не участвует: (0.52 + 1) / 2.
  EXPECT_NEAR(m.hhiAvg, 0.76, kTol);
  EXPECT_NEAR(m.maxPositionShare, 0.4, kTol);  // max(600/2000, 1000/2500)
}

TEST(Metrics, NoPositionsNoConcentration) {
  MetricsInput in = fiveDays();
  EXPECT_EQ(core::computeReport(in).metrics.hhiAvg, 0.0);
  in.positionValuesByStep.assign(5, {D("0")});
  const core::Metrics m = core::computeReport(in).metrics;
  EXPECT_EQ(m.hhiAvg, 0.0);
  EXPECT_EQ(m.maxPositionShare, 0.0);
}

TEST(Metrics, Benchmark) {
  MetricsInput in = fiveDays();
  EXPECT_FALSE(core::computeReport(in).metrics.benchmarkReturn);
  in.benchmark = std::make_pair(D("2500.5"), D("2750.55"));
  const core::Metrics m = core::computeReport(in).metrics;
  ASSERT_TRUE(m.benchmarkReturn);
  EXPECT_NEAR(*m.benchmarkReturn, 2750.55 / 2500.5 - 1.0, kTol);
}

TEST(Metrics, RejectsInvalidInput) {
  MetricsInput bad = fiveDays();
  bad.initialCash = D("0");
  EXPECT_THROW(core::computeReport(bad), std::invalid_argument);

  bad = fiveDays();
  bad.tradingDayOfStep.pop_back();  // шаг 4 вне шкалы
  EXPECT_THROW(core::computeReport(bad), std::invalid_argument);

  bad = fiveDays();
  bad.equity[2].step = 1;  // не по возрастанию
  EXPECT_THROW(core::computeReport(bad), std::invalid_argument);

  bad = fiveDays();
  bad.positionValuesByStep.assign(4, {D("1")});  // короче шага 4
  EXPECT_THROW(core::computeReport(bad), std::invalid_argument);
}

TEST(Metrics, Deterministic) {
  const core::Report a = core::computeReport(fiveDays());
  const core::Report b = core::computeReport(fiveDays());
  EXPECT_EQ(a.metrics.sharpe, b.metrics.sharpe);
  EXPECT_EQ(a.metrics.volatility, b.metrics.volatility);
}

}  // namespace
