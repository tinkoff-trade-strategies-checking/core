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

Decimal d(const char *Text) { return Decimal::parse(Text); }

// Дневной ряд: по шагу на день, шаг i - день i.
MetricsInput dailyInput(const std::vector<const char *> &Equity,
                        const char *InitialCash = "1000000") {
  MetricsInput In;
  In.InitialCash = d(InitialCash);
  for (std::size_t I = 0; I < Equity.size(); ++I) {
    In.Equity.push_back({static_cast<std::int64_t>(I), d(Equity[I])});
    In.TradingDayOfStep.push_back(static_cast<std::int32_t>(I));
  }
  return In;
}

MetricsInput fiveDays() {
  return dailyInput({"1010000", "1005000", "1020000", "990000", "1030000"});
}

Fill fill(const char *Value, const char *Commission, const char *Fee,
          const char *Spread = "0", const char *Slippage = "0") {
  Fill F;
  F.Value = d(Value);
  F.Commission = d(Commission);
  F.Fee = d(Fee);
  F.SpreadCost = d(Spread);
  F.SlippageCost = d(Slippage);
  return F;
}

RoundTrip trip(const char *NetPnl) {
  RoundTrip T{};
  T.NetPnl = d(NetPnl);
  return T;
}

TEST(Metrics, DailySeries) {
  const core::Report R = core::computeReport(fiveDays());
  const core::Metrics &M = R.Stats;
  EXPECT_NEAR(M.TotalReturn, 0.03, kTol);
  ASSERT_TRUE(M.Cagr);
  EXPECT_NEAR(*M.Cagr, 3.4360468599, kTol);
  ASSERT_TRUE(M.Volatility);
  EXPECT_NEAR(*M.Volatility, 0.4088278812, kTol);
  ASSERT_TRUE(M.Sharpe);
  EXPECT_NEAR(*M.Sharpe, 3.8176079043, kTol);
  ASSERT_TRUE(M.Sortino);
  EXPECT_NEAR(*M.Sortino, 7.3710457334, kTol);
  EXPECT_NEAR(M.MaxDrawdown, 0.0294117647, kTol);
  EXPECT_EQ(M.MaxDrawdownSteps, 1);
  EXPECT_EQ(M.TradingDays, 5);
  EXPECT_EQ(R.FinalEquity, d("1030000"));
  EXPECT_EQ(R.Equity.size(), 5u);
}

TEST(Metrics, OneDayHasNoVolatility) {
  const core::Metrics M = core::computeReport(dailyInput({"1010000"})).Stats;
  EXPECT_EQ(M.TradingDays, 1);
  EXPECT_FALSE(M.Volatility);
  EXPECT_FALSE(M.Sharpe);
  EXPECT_FALSE(M.Sortino);
  ASSERT_TRUE(M.Cagr);
  EXPECT_NEAR(*M.Cagr, std::pow(1.01, 252.0) - 1.0, 1e-6);
}

TEST(Metrics, OneLosingDayHasNoSortino) {
  EXPECT_FALSE(core::computeReport(dailyInput({"990000"})).Stats.Sortino);
}

TEST(Metrics, NoEquityPoints) {
  MetricsInput In;
  In.InitialCash = d("1000000");
  const core::Report R = core::computeReport(In);
  EXPECT_EQ(R.FinalEquity, d("1000000"));
  EXPECT_EQ(R.Stats.TradingDays, 0);
  EXPECT_FALSE(R.Stats.Cagr);
  EXPECT_FALSE(R.Stats.Volatility);
  EXPECT_EQ(R.Stats.TotalReturn, 0.0);
  EXPECT_EQ(R.Stats.MaxDrawdown, 0.0);
  EXPECT_EQ(R.Stats.Turnover, 0.0);
}

TEST(Metrics, ConstantReturnHasNoSharpeAndSortino) {
  // Доходность каждый день одинаковая: разброса нет.
  const core::Metrics M =
      core::computeReport(dailyInput({"1100000", "1210000", "1331000"})).Stats;
  ASSERT_TRUE(M.Volatility);
  EXPECT_NEAR(*M.Volatility, 0.0, 1e-9);
  EXPECT_FALSE(M.Sharpe);
  EXPECT_FALSE(M.Sortino); // нет убыточных дней
}

TEST(Metrics, TinySpreadHasNoSharpeAndSortino) {
  // Разброс порядка 1e-15 - шум округления double, не настоящий разброс: Sharpe
  // и Sortino не определены.
  MetricsInput In = dailyInput({"1000000.000000001", "1000000.000000002",
                                "1000000.000000003", "999999.999999999"});
  const core::Metrics M = core::computeReport(In).Stats;
  EXPECT_FALSE(M.Sharpe);
  EXPECT_FALSE(M.Sortino);
  ASSERT_TRUE(M.Volatility);
  EXPECT_TRUE(std::isfinite(*M.Volatility));
}

TEST(Metrics, BankruptHasFiniteMetrics) {
  MetricsInput In = dailyInput({"500000", "0"});
  In.Fills = {fill("100000", "0", "0")};
  const core::Metrics M = core::computeReport(In).Stats;
  EXPECT_FALSE(M.Cagr); // итоговая стоимость 0
  EXPECT_NEAR(M.TotalReturn, -1.0, kTol);
  EXPECT_NEAR(M.Turnover, 100000.0 / 250000.0, kTol);
  EXPECT_TRUE(std::isfinite(M.MaxDrawdown));

  In = dailyInput({"0", "0"});
  In.Fills = {fill("100000", "0", "0")};
  const core::Metrics Z = core::computeReport(In).Stats;
  EXPECT_EQ(Z.Turnover, 0.0); // средняя стоимость 0
  EXPECT_FALSE(Z.Cagr);
  EXPECT_TRUE(std::isfinite(Z.TotalReturn));
}

TEST(Metrics, LastStepOfDayDefinesDailyReturn) {
  MetricsInput In;
  In.InitialCash = d("1000000");
  In.TradingDayOfStep = {0, 0, 0, 1, 1, 1};
  const char *Values[] = {"1000500", "1001000", "1002000",
                          "1003000", "1000000", "1004000"};
  for (int I = 0; I < 6; ++I)
    In.Equity.push_back({I, d(Values[I])});
  const core::Metrics M = core::computeReport(In).Stats;
  EXPECT_EQ(M.TradingDays, 2);
  const double R1 = 1002000.0 / 1000000.0 - 1.0,
               R2 = 1004000.0 / 1002000.0 - 1.0;
  const double Mu = (R1 + R2) / 2;
  const double Sd =
      std::sqrt(((R1 - Mu) * (R1 - Mu) + (R2 - Mu) * (R2 - Mu)) / 1.0);
  ASSERT_TRUE(M.Volatility);
  EXPECT_NEAR(*M.Volatility, Sd * std::sqrt(252.0), kTol);
  // Просадка считается по шагам, а не по дням: шаг 4 (1 000 000) ниже пика 1
  // 003 000.
  EXPECT_NEAR(M.MaxDrawdown, 1.0 - 1000000.0 / 1003000.0, kTol);
}

TEST(Metrics, DrawdownFromInitialCashAndRunLength) {
  // Пик - начальный капитал; ниже пика три шага подряд, затем новый максимум,
  // затем один шаг ниже.
  const core::Metrics M =
      core::computeReport(
          dailyInput({"990000", "980000", "985000", "1010000", "1000000"}))
          .Stats;
  EXPECT_NEAR(M.MaxDrawdown, 0.02, kTol);
  EXPECT_EQ(M.MaxDrawdownSteps, 3);
}

TEST(Metrics, ReturnToPeakEndsRun) {
  const core::Metrics M =
      core::computeReport(dailyInput({"990000", "1000000", "990000", "980000"}))
          .Stats;
  EXPECT_EQ(M.MaxDrawdownSteps, 2);
}

TEST(Metrics, CostsSummed) {
  MetricsInput In = fiveDays();
  // Сделки 1-3 таблицы эталонного портфеля (задача 16).
  In.Fills = {fill("271540.00", "135.77", "27.16", "100.00", "5.00"),
              fill("134455.00", "67.23", "13.45", "50.00", "2.50"),
              fill("73575.00", "36.79", "7.36", "20.00", "0")};
  const core::Report R = core::computeReport(In);
  EXPECT_EQ(R.Costs.Commission, d("239.79"));
  EXPECT_EQ(R.Costs.Fee, d("47.97"));
  EXPECT_EQ(R.Costs.Spread, d("170.00"));
  EXPECT_EQ(R.Costs.Slippage, d("7.50"));
  EXPECT_EQ(R.Stats.Fills, 3);
  // grossReturn = totalReturn + (комиссии + сборы) / E0.
  EXPECT_NEAR(R.Stats.GrossReturn, 0.03 + (239.79 + 47.97) / 1000000.0, 1e-12);
}

TEST(Metrics, TurnoverIsTradedValueOverMeanDailyEquity) {
  MetricsInput In = fiveDays();
  In.Fills = {fill("100000", "0", "0"), fill("102200", "0", "0")};
  // mean(E_d) = 1 011 000
  EXPECT_NEAR(core::computeReport(In).Stats.Turnover, 202200.0 / 1011000.0,
              kTol);
}

TEST(Metrics, WinRateAndCounters) {
  MetricsInput In = fiveDays();
  EXPECT_FALSE(core::computeReport(In).Stats.WinRate);
  In.RoundTrips = {trip("100.50"), trip("-20"), trip("0"), trip("5")};
  In.Rejections = 3;
  In.PartialFills = 2;
  const core::Metrics M = core::computeReport(In).Stats;
  ASSERT_TRUE(M.WinRate);
  EXPECT_NEAR(*M.WinRate, 0.5, kTol); // ноль - не выигрыш
  EXPECT_EQ(M.RoundTrips, 4);
  EXPECT_EQ(M.Rejections, 3);
  EXPECT_EQ(M.PartialFills, 2);
}

TEST(Metrics, ConcentrationByPositionValues) {
  MetricsInput In = dailyInput({"2000", "2000", "2500"}, "2000");
  In.PositionValuesByStep = {
      {d("600"), d("400")}, {d("0"), d("0")}, {d("1000"), d("0")}};
  const core::Metrics M = core::computeReport(In).Stats;
  // Шаг без позиций в среднем HHI не участвует: (0.52 + 1) / 2.
  EXPECT_NEAR(M.HhiAvg, 0.76, kTol);
  EXPECT_NEAR(M.MaxPositionShare, 0.4, kTol); // max(600/2000, 1000/2500)
}

TEST(Metrics, NoPositionsNoConcentration) {
  MetricsInput In = fiveDays();
  EXPECT_EQ(core::computeReport(In).Stats.HhiAvg, 0.0);
  In.PositionValuesByStep.assign(5, {d("0")});
  const core::Metrics M = core::computeReport(In).Stats;
  EXPECT_EQ(M.HhiAvg, 0.0);
  EXPECT_EQ(M.MaxPositionShare, 0.0);
}

TEST(Metrics, Benchmark) {
  MetricsInput In = fiveDays();
  EXPECT_FALSE(core::computeReport(In).Stats.BenchmarkReturn);
  In.Benchmark = std::make_pair(d("2500.5"), d("2750.55"));
  const core::Metrics M = core::computeReport(In).Stats;
  ASSERT_TRUE(M.BenchmarkReturn);
  EXPECT_NEAR(*M.BenchmarkReturn, 2750.55 / 2500.5 - 1.0, kTol);
}

TEST(Metrics, RejectsInvalidInput) {
  MetricsInput Bad = fiveDays();
  Bad.InitialCash = d("0");
  EXPECT_THROW(core::computeReport(Bad), std::invalid_argument);

  Bad = fiveDays();
  Bad.TradingDayOfStep.pop_back(); // шаг 4 вне шкалы
  EXPECT_THROW(core::computeReport(Bad), std::invalid_argument);

  Bad = fiveDays();
  Bad.Equity[2].Step = 1; // не по возрастанию
  EXPECT_THROW(core::computeReport(Bad), std::invalid_argument);

  Bad = fiveDays();
  Bad.PositionValuesByStep.assign(4, {d("1")}); // короче шага 4
  EXPECT_THROW(core::computeReport(Bad), std::invalid_argument);
}

TEST(Metrics, Deterministic) {
  const core::Report A = core::computeReport(fiveDays());
  const core::Report B = core::computeReport(fiveDays());
  EXPECT_EQ(A.Stats.Sharpe, B.Stats.Sharpe);
  EXPECT_EQ(A.Stats.Volatility, B.Stats.Volatility);
}

} // namespace
