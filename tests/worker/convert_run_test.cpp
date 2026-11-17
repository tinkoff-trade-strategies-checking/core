#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

#include "convert/report.hpp"
#include "convert/run.hpp"
#include "core/market/timeline.hpp"
#include "core/report/metrics.hpp"
#include "core/run/backtest.hpp"
#include "support/fixture1.hpp"

namespace {

namespace pb = pac::simulation::v1;

core::Decimal d(const char *Text) { return core::Decimal::parse(Text); }

core::Timeline fixtureTimeline() {
  auto F = core::testing::fixture1();
  return core::Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                              core::Timeframe::D1, 20240109, 20240122);
}

core::Fill makeFill(std::int64_t OrderId, std::int64_t Step, int Instrument,
                    core::Side Side, std::int64_t Lots, const char *Price) {
  core::Fill F;
  F.OrderId = OrderId;
  F.Step = Step;
  F.Instrument = Instrument;
  F.OrderSide = Side;
  F.Lots = Lots;
  F.Price = d(Price);
  F.BasePrice = d("100");
  F.Value = d("1000.25");
  F.Commission = d("0.50");
  F.Fee = d("0.10");
  F.SpreadCost = d("0.5");
  F.SlippageCost = d("-0.01");
  F.StopAndTakeSameBar = Side == core::Side::Sell;
  return F;
}

core::RunOutput makeOutput() {
  core::RunOutput Out;
  Out.Status = core::RunStatus::BankruptStop;
  Out.Severity = core::dsl::Level::Warning;
  Out.LastStep = 7;
  Out.Fills = {makeFill(0, 1, 0, core::Side::Buy, 5, "100.123456789"),
               makeFill(2, 6, 1, core::Side::Sell, 3, "0.02")};
  core::Order Order;
  Order.Id = 0;
  Order.Spec.Instrument = 0;
  Order.Spec.Lots = 5;
  Order.Status = core::OrderStatus::Filled;
  Order.FilledLots = 5;
  Order.RuleName = "r1";
  Order.Because = "пересечение";
  Out.Orders = {Order};
  Out.Equity = {{0, d("1000000")}, {1, d("999000.5")}};
  Out.Findings = {
      core::Finding{"W-RT01", core::dsl::Level::Warning, "сообщение", 4}};
  Out.Digest = 0xDEADBEEFCAFEF00DULL;
  Out.FinalEquity = d("999000.5");
  return Out;
}

core::CostBreakdown makeBreakdown() {
  core::CostBreakdown Cb;
  const core::CostFactor Factors[] = {
      core::CostFactor::Naive,    core::CostFactor::Delay,
      core::CostFactor::Fees,     core::CostFactor::Spread,
      core::CostFactor::Slippage, core::CostFactor::Liquidity};
  const char *const Results[] = {"150000.5", "140000",          "120000.25",
                                 "100000",   "80000.000000001", "-0.5"};
  const double Returns[] = {0.15, 0.14, 0.12, 0.10, 0.08, -0.0000005};
  for (int I = 0; I < 6; ++I)
    Cb.Steps.push_back({Factors[I], d(Results[I]), Returns[I]});
  Cb.Flags = {core::CostFlag::CostsOverHalfProfit, core::CostFlag::SharpeOver3};
  return Cb;
}

} // namespace

TEST(ConvertRun, RoundTrip) {
  const core::Timeline Timeline = fixtureTimeline();
  const core::RunOutput Output = makeOutput();
  core::MetricsInput In =
      worker::metricsInput(Output, d("1000000"), Timeline, std::nullopt);
  const core::Report Report = core::computeReport(In);

  pb::RunResult Result;
  worker::toProto(Output, Report, makeBreakdown(), Timeline, &Result);

  EXPECT_EQ(Result.status(), pb::BANKRUPT_STOP);
  EXPECT_EQ(Result.level(), pb::WARNING);
  EXPECT_EQ(Result.digest(), 0xDEADBEEFCAFEF00DULL);
  EXPECT_EQ(Result.last_step(), 7);
  EXPECT_EQ(Result.total_steps(), 10);

  // Деньги - пары {units, nano}, отрицательные - с отрицательным nano.
  ASSERT_EQ(Result.fills_size(), 2);
  const pb::Fill &Buy = Result.fills(0);
  EXPECT_EQ(Buy.order_id(), 0);
  EXPECT_EQ(Buy.step(), 1);
  EXPECT_EQ(Buy.instrument_key(), "A");
  EXPECT_EQ(Buy.side(), pb::BUY);
  EXPECT_EQ(Buy.lots(), 5);
  EXPECT_EQ(Buy.price().units(), 100);
  EXPECT_EQ(Buy.price().nano(), 123456789);
  EXPECT_EQ(Buy.value().units(), 1000);
  EXPECT_EQ(Buy.value().nano(), 250000000);
  EXPECT_EQ(Buy.commission().nano(), 500000000);
  EXPECT_EQ(Buy.exchange_fee().nano(), 100000000);
  EXPECT_EQ(Buy.slippage_cost().units(), 0);
  EXPECT_EQ(Buy.slippage_cost().nano(), -10000000);
  EXPECT_FALSE(Buy.stop_and_take_same_bar());
  const pb::Fill &Sell = Result.fills(1);
  EXPECT_EQ(Sell.instrument_key(), "B");
  EXPECT_EQ(Sell.side(), pb::SELL);
  EXPECT_TRUE(Sell.stop_and_take_same_bar());

  ASSERT_EQ(Result.orders_size(), 1);
  EXPECT_EQ(Result.orders(0).rule_name(), "r1");
  EXPECT_EQ(Result.orders(0).because(), "пересечение");
  EXPECT_EQ(Result.orders(0).status(), pb::FILLED);

  // Отчёт: кривая стоимости, итог и находки прогона.
  const pb::Report &R = Result.report();
  EXPECT_EQ(R.level(), pb::WARNING);
  ASSERT_EQ(R.findings_size(), 1);
  EXPECT_EQ(R.findings(0).code(), "W-RT01");
  EXPECT_EQ(R.findings(0).level(), pb::WARNING);
  EXPECT_EQ(R.findings(0).step(), 4);
  ASSERT_EQ(R.equity_size(), 2);
  EXPECT_EQ(R.equity(1).step(), 1);
  EXPECT_EQ(R.equity(1).equity().units(), 999000);
  EXPECT_EQ(R.equity(1).equity().nano(), 500000000);
  EXPECT_EQ(R.final_equity().units(), 999000);
  EXPECT_EQ(R.metrics().fills(), 2);
  EXPECT_NEAR(R.metrics().total_return(), 999000.5 / 1000000.0 - 1.0, 1e-12);
  EXPECT_FALSE(R.metrics().has_benchmark_return());
  EXPECT_EQ(R.costs().commission().units(), 1); // 0.50 + 0.50

  // Цепочка издержек: шесть шагов в порядке ядра.
  ASSERT_TRUE(Result.has_cost_breakdown());
  const pb::CostBreakdown &Cb = Result.cost_breakdown();
  ASSERT_EQ(Cb.steps_size(), 6);
  const pb::CostFactor Order[] = {pb::NAIVE,  pb::DELAY,    pb::FEES,
                                  pb::SPREAD, pb::SLIPPAGE, pb::LIQUIDITY};
  for (int I = 0; I < 6; ++I)
    EXPECT_EQ(Cb.steps(I).factor(), Order[I]) << I;
  EXPECT_EQ(Cb.steps(0).net_result().units(), 150000);
  EXPECT_EQ(Cb.steps(0).net_result().nano(), 500000000);
  EXPECT_EQ(Cb.steps(4).net_result().units(), 80000);
  EXPECT_EQ(Cb.steps(4).net_result().nano(), 1);
  EXPECT_EQ(Cb.steps(5).net_result().units(), 0);
  EXPECT_EQ(Cb.steps(5).net_result().nano(), -500000000);
  EXPECT_DOUBLE_EQ(Cb.steps(0).net_return(), 0.15);
  ASSERT_EQ(Cb.flags_size(), 2);
  EXPECT_EQ(Cb.flags(0), pb::COSTS_OVER_HALF_PROFIT);
  EXPECT_EQ(Cb.flags(1), pb::SHARPE_OVER_3);
}

TEST(ConvertRun, NoBreakdownWhenNotComputed) {
  const core::Timeline Timeline = fixtureTimeline();
  core::RunOutput Output = makeOutput();
  Output.Status = core::RunStatus::Completed;
  Output.Severity = core::dsl::Level::Ok;
  Output.Findings.clear();
  const core::Report Report = core::computeReport(
      worker::metricsInput(Output, d("1000000"), Timeline, std::nullopt));

  pb::RunResult Result;
  worker::toProto(Output, Report, std::nullopt, Timeline, &Result);
  EXPECT_EQ(Result.status(), pb::COMPLETED);
  EXPECT_EQ(Result.level(), pb::OK);
  EXPECT_FALSE(Result.has_cost_breakdown());
  EXPECT_EQ(Result.report().findings_size(), 0);
}

TEST(ConvertRun, InvalidRunStatus) {
  EXPECT_EQ(worker::toProto(core::RunStatus::Invalid), pb::INVALID);
}

// Один и тот же результат даёт побайтно одинаковое сообщение (INV-4).
TEST(ConvertRun, Deterministic) {
  const core::Timeline Timeline = fixtureTimeline();
  const core::RunOutput Output = makeOutput();
  const core::Report Report = core::computeReport(
      worker::metricsInput(Output, d("1000000"), Timeline, std::nullopt));
  pb::RunResult A;
  pb::RunResult B;
  worker::toProto(Output, Report, makeBreakdown(), Timeline, &A);
  worker::toProto(Output, Report, makeBreakdown(), Timeline, &B);
  EXPECT_EQ(A.SerializeAsString(), B.SerializeAsString());
}
