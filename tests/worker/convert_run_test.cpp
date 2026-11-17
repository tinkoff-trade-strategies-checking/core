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

core::Decimal D(const char *text) { return core::Decimal::parse(text); }

core::Timeline fixtureTimeline() {
  auto f = core::testing::fixture1();
  return core::Timeline::load(f.bars, f.calendar, f.snapshotId, f.instruments,
                              core::Timeframe::D1, 20240109, 20240122);
}

core::Fill makeFill(std::int64_t orderId, std::int64_t step, int instrument,
                    core::Side side, std::int64_t lots, const char *price) {
  core::Fill f;
  f.orderId = orderId;
  f.step = step;
  f.instrument = instrument;
  f.side = side;
  f.lots = lots;
  f.price = D(price);
  f.basePrice = D("100");
  f.value = D("1000.25");
  f.commission = D("0.50");
  f.fee = D("0.10");
  f.spreadCost = D("0.5");
  f.slippageCost = D("-0.01");
  f.stopAndTakeSameBar = side == core::Side::Sell;
  return f;
}

core::RunOutput makeOutput() {
  core::RunOutput out;
  out.status = core::RunStatus::BankruptStop;
  out.level = core::dsl::Level::Warning;
  out.lastStep = 7;
  out.fills = {makeFill(0, 1, 0, core::Side::Buy, 5, "100.123456789"),
               makeFill(2, 6, 1, core::Side::Sell, 3, "0.02")};
  core::Order order;
  order.id = 0;
  order.spec.instrument = 0;
  order.spec.lots = 5;
  order.status = core::OrderStatus::Filled;
  order.filledLots = 5;
  order.ruleName = "r1";
  order.because = "пересечение";
  out.orders = {order};
  out.equity = {{0, D("1000000")}, {1, D("999000.5")}};
  out.findings = {
      core::Finding{"W-RT01", core::dsl::Level::Warning, "сообщение", 4}};
  out.digest = 0xDEADBEEFCAFEF00DULL;
  out.finalEquity = D("999000.5");
  return out;
}

core::CostBreakdown makeBreakdown() {
  core::CostBreakdown cb;
  const core::CostFactor factors[] = {
      core::CostFactor::Naive,    core::CostFactor::Delay,
      core::CostFactor::Fees,     core::CostFactor::Spread,
      core::CostFactor::Slippage, core::CostFactor::Liquidity};
  const char *const results[] = {"150000.5", "140000",          "120000.25",
                                 "100000",   "80000.000000001", "-0.5"};
  const double returns[] = {0.15, 0.14, 0.12, 0.10, 0.08, -0.0000005};
  for (int i = 0; i < 6; ++i)
    cb.steps.push_back({factors[i], D(results[i]), returns[i]});
  cb.flags = {core::CostFlag::CostsOverHalfProfit, core::CostFlag::SharpeOver3};
  return cb;
}

} // namespace

TEST(ConvertRun, RoundTrip) {
  const core::Timeline timeline = fixtureTimeline();
  const core::RunOutput output = makeOutput();
  core::MetricsInput in =
      worker::metricsInput(output, D("1000000"), timeline, std::nullopt);
  const core::Report report = core::computeReport(in);

  pb::RunResult result;
  worker::toProto(output, report, makeBreakdown(), timeline, &result);

  EXPECT_EQ(result.status(), pb::BANKRUPT_STOP);
  EXPECT_EQ(result.level(), pb::WARNING);
  EXPECT_EQ(result.digest(), 0xDEADBEEFCAFEF00DULL);
  EXPECT_EQ(result.last_step(), 7);
  EXPECT_EQ(result.total_steps(), 10);

  // Деньги - пары {units, nano}, отрицательные - с отрицательным nano.
  ASSERT_EQ(result.fills_size(), 2);
  const pb::Fill &buy = result.fills(0);
  EXPECT_EQ(buy.order_id(), 0);
  EXPECT_EQ(buy.step(), 1);
  EXPECT_EQ(buy.instrument_key(), "A");
  EXPECT_EQ(buy.side(), pb::BUY);
  EXPECT_EQ(buy.lots(), 5);
  EXPECT_EQ(buy.price().units(), 100);
  EXPECT_EQ(buy.price().nano(), 123456789);
  EXPECT_EQ(buy.value().units(), 1000);
  EXPECT_EQ(buy.value().nano(), 250000000);
  EXPECT_EQ(buy.commission().nano(), 500000000);
  EXPECT_EQ(buy.exchange_fee().nano(), 100000000);
  EXPECT_EQ(buy.slippage_cost().units(), 0);
  EXPECT_EQ(buy.slippage_cost().nano(), -10000000);
  EXPECT_FALSE(buy.stop_and_take_same_bar());
  const pb::Fill &sell = result.fills(1);
  EXPECT_EQ(sell.instrument_key(), "B");
  EXPECT_EQ(sell.side(), pb::SELL);
  EXPECT_TRUE(sell.stop_and_take_same_bar());

  ASSERT_EQ(result.orders_size(), 1);
  EXPECT_EQ(result.orders(0).rule_name(), "r1");
  EXPECT_EQ(result.orders(0).because(), "пересечение");
  EXPECT_EQ(result.orders(0).status(), pb::FILLED);

  // Отчёт: кривая стоимости, итог и находки прогона.
  const pb::Report &r = result.report();
  EXPECT_EQ(r.level(), pb::WARNING);
  ASSERT_EQ(r.findings_size(), 1);
  EXPECT_EQ(r.findings(0).code(), "W-RT01");
  EXPECT_EQ(r.findings(0).level(), pb::WARNING);
  EXPECT_EQ(r.findings(0).step(), 4);
  ASSERT_EQ(r.equity_size(), 2);
  EXPECT_EQ(r.equity(1).step(), 1);
  EXPECT_EQ(r.equity(1).equity().units(), 999000);
  EXPECT_EQ(r.equity(1).equity().nano(), 500000000);
  EXPECT_EQ(r.final_equity().units(), 999000);
  EXPECT_EQ(r.metrics().fills(), 2);
  EXPECT_NEAR(r.metrics().total_return(), 999000.5 / 1000000.0 - 1.0, 1e-12);
  EXPECT_FALSE(r.metrics().has_benchmark_return());
  EXPECT_EQ(r.costs().commission().units(), 1); // 0.50 + 0.50

  // Цепочка издержек: шесть шагов в порядке ядра.
  ASSERT_TRUE(result.has_cost_breakdown());
  const pb::CostBreakdown &cb = result.cost_breakdown();
  ASSERT_EQ(cb.steps_size(), 6);
  const pb::CostFactor order[] = {pb::NAIVE,  pb::DELAY,    pb::FEES,
                                  pb::SPREAD, pb::SLIPPAGE, pb::LIQUIDITY};
  for (int i = 0; i < 6; ++i)
    EXPECT_EQ(cb.steps(i).factor(), order[i]) << i;
  EXPECT_EQ(cb.steps(0).net_result().units(), 150000);
  EXPECT_EQ(cb.steps(0).net_result().nano(), 500000000);
  EXPECT_EQ(cb.steps(4).net_result().units(), 80000);
  EXPECT_EQ(cb.steps(4).net_result().nano(), 1);
  EXPECT_EQ(cb.steps(5).net_result().units(), 0);
  EXPECT_EQ(cb.steps(5).net_result().nano(), -500000000);
  EXPECT_DOUBLE_EQ(cb.steps(0).net_return(), 0.15);
  ASSERT_EQ(cb.flags_size(), 2);
  EXPECT_EQ(cb.flags(0), pb::COSTS_OVER_HALF_PROFIT);
  EXPECT_EQ(cb.flags(1), pb::SHARPE_OVER_3);
}

TEST(ConvertRun, NoBreakdownWhenNotComputed) {
  const core::Timeline timeline = fixtureTimeline();
  core::RunOutput output = makeOutput();
  output.status = core::RunStatus::Completed;
  output.level = core::dsl::Level::Ok;
  output.findings.clear();
  const core::Report report = core::computeReport(
      worker::metricsInput(output, D("1000000"), timeline, std::nullopt));

  pb::RunResult result;
  worker::toProto(output, report, std::nullopt, timeline, &result);
  EXPECT_EQ(result.status(), pb::COMPLETED);
  EXPECT_EQ(result.level(), pb::OK);
  EXPECT_FALSE(result.has_cost_breakdown());
  EXPECT_EQ(result.report().findings_size(), 0);
}

TEST(ConvertRun, InvalidRunStatus) {
  EXPECT_EQ(worker::toProto(core::RunStatus::Invalid), pb::INVALID);
}

// Один и тот же результат даёт побайтно одинаковое сообщение (INV-4).
TEST(ConvertRun, Deterministic) {
  const core::Timeline timeline = fixtureTimeline();
  const core::RunOutput output = makeOutput();
  const core::Report report = core::computeReport(
      worker::metricsInput(output, D("1000000"), timeline, std::nullopt));
  pb::RunResult a;
  pb::RunResult b;
  worker::toProto(output, report, makeBreakdown(), timeline, &a);
  worker::toProto(output, report, makeBreakdown(), timeline, &b);
  EXPECT_EQ(a.SerializeAsString(), b.SerializeAsString());
}
