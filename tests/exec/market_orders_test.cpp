#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/exec/costs.hpp"
#include "core/exec/execution.hpp"
#include "core/market/timeline.hpp"

namespace {

using core::Bar;
using core::CostConfig;
using core::Decimal;
using core::ExecutionEngine;
using core::Fill;
using core::Instrument;
using core::Order;
using core::OrderStatus;
using core::OrderType;
using core::Portfolio;
using core::Reason;
using core::Side;
using core::Timeline;

Decimal D(const char* text) { return Decimal::parse(text); }

constexpr std::int64_t kMinuteMs = 60'000;
constexpr std::int64_t kStart = 1'704'747'600'000 + 10 * 3'600'000;  // 2024-01-09 10:00 МСК

const Instrument kA{"A", "FIXA", 10, Decimal::parse("0.01")};
const Instrument kB{"B", "FIXB", 10000, Decimal::parse("0.000005")};

// Бар минутной шкалы: номер минуты t, цены и объём в лотах (lotSize нужен для пересчёта в штуки).
struct B {
  int t;
  const char* open;
  const char* close;
  std::int64_t volumeLots;
};

std::vector<Bar> bars(const Instrument& inst, const std::vector<B>& list) {
  std::vector<Bar> out;
  for (const B& b : list) {
    Bar bar;
    bar.tsOpenMs = kStart + b.t * kMinuteMs;
    bar.availableAtMs = bar.tsOpenMs + kMinuteMs;
    bar.open = D(b.open);
    bar.close = D(b.close);
    bar.high = bar.low = bar.open;
    if (bar.close > bar.high) bar.high = bar.close;
    if (bar.close < bar.low) bar.low = bar.close;
    bar.volume = b.volumeLots * inst.lotSize;
    out.push_back(bar);
  }
  return out;
}

// Бумага A - бары на шагах 0…5 (open 100 + шаг, close open + 0.5); V - объём каждого бара.
std::vector<B> seriesA(std::int64_t volumeLots) {
  static const char* opens[] = {"100", "101", "102", "103", "104", "105"};
  static const char* closes[] = {"100.5", "101.5", "102.5", "103.5", "104.5", "105.5"};
  std::vector<B> out;
  for (int i = 0; i < 6; ++i) out.push_back(B{i, opens[i], closes[i], volumeLots});
  return out;
}

Order market(std::int64_t id, int instrument, Side side, std::int64_t lots, std::int64_t placedStep) {
  Order o;
  o.id = id;
  o.spec.instrument = instrument;
  o.spec.side = side;
  o.spec.type = OrderType::Market;
  o.spec.lots = lots;
  o.placedStep = placedStep;
  return o;
}

struct Book {
  std::vector<Order> orders;
  std::vector<Fill> fills;
  std::vector<std::int64_t> changed;
};

TEST(MarketOrders, NotBeforeNextBar) {
  // INV-2: заявка шага 0 исполняется на шаге 1 по open шага 1 (101), а не по close шага 0.
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(50000))}, {});
  const ExecutionEngine engine({kA}, CostConfig::defaults(), false);
  Portfolio pf(D("1000000"), 1);
  Book r;
  r.orders.push_back(market(0, 0, Side::Buy, 100, 0));

  engine.processStep(0, t, r.orders, pf, r.fills, r.changed);
  EXPECT_TRUE(r.fills.empty());
  EXPECT_TRUE(r.changed.empty());
  EXPECT_EQ(r.orders[0].status, OrderStatus::Active);
  EXPECT_EQ(pf.lots(0), 0);

  engine.processStep(1, t, r.orders, pf, r.fills, r.changed);
  ASSERT_EQ(r.fills.size(), 1u);
  const Fill& f = r.fills[0];
  EXPECT_EQ(f.orderId, 0);
  EXPECT_EQ(f.step, 1);
  EXPECT_EQ(f.basePrice, D("101"));
  EXPECT_EQ(f.price, D("101.08"));  // 101 * 1.0007 = 101.0707 -> шаг 0.01 вверх
  EXPECT_EQ(f.lots, 100);
  EXPECT_EQ(r.orders[0].status, OrderStatus::Filled);
  EXPECT_EQ(r.orders[0].reason, Reason::None);
  EXPECT_EQ(r.orders[0].filledLots, 100);
  EXPECT_EQ(r.orders[0].closedStep, 1);
  ASSERT_EQ(r.changed.size(), 1u);
  EXPECT_EQ(r.changed[0], 0);
  EXPECT_EQ(pf.lots(0), 100);
}

TEST(MarketOrders, FillRecordsAmountsAndCostSplit) {
  std::vector<B> a = seriesA(50000);
  a[1].open = "271.35";
  const Timeline t = Timeline::build({kA}, {bars(kA, a)}, {});
  const ExecutionEngine engine({kA}, CostConfig::defaults(), false);
  Portfolio pf(D("1000000"), 1);
  Book r;
  r.orders.push_back(market(0, 0, Side::Buy, 100, 0));
  engine.processStep(1, t, r.orders, pf, r.fills, r.changed);
  ASSERT_EQ(r.fills.size(), 1u);
  const Fill& f = r.fills[0];
  EXPECT_EQ(f.price, D("271.54"));
  EXPECT_EQ(f.value, D("271540.00"));
  EXPECT_EQ(f.commission, D("135.77"));
  EXPECT_EQ(f.fee, D("27.16"));
  EXPECT_EQ(f.spreadCost, D("135.68"));
  EXPECT_EQ(f.slippageCost, D("54.32"));
  EXPECT_FALSE(f.stopAndTakeSameBar);
  EXPECT_EQ(pf.cash(), D("1000000") - D("271540.00") - D("135.77") - D("27.16"));
}

TEST(MarketOrders, WaitsForOwnInstrumentBar) {
  // У B нет бара на шаге 3: заявка B шага 2 исполняется на шаге 4, по open бара B, не по бару A.
  std::vector<B> b = {{0, "0.0240", "0.0240", 90000}, {1, "0.0242", "0.0242", 90000},
                      {2, "0.0244", "0.0244", 90000}, {4, "0.0245", "0.0245", 90000},
                      {5, "0.0246", "0.0246", 90000}};
  const Timeline t = Timeline::build({kA, kB}, {bars(kA, seriesA(50000)), bars(kB, b)}, {});
  ASSERT_EQ(t.barOf(1, 3), nullptr);
  const ExecutionEngine engine({kA, kB}, CostConfig::defaults(), false);
  Portfolio pf(D("1000000"), 2);
  Book r;
  r.orders.push_back(market(0, 1, Side::Buy, 300, 2));

  engine.processStep(3, t, r.orders, pf, r.fills, r.changed);
  EXPECT_TRUE(r.fills.empty());
  EXPECT_EQ(r.orders[0].status, OrderStatus::Active);

  engine.processStep(4, t, r.orders, pf, r.fills, r.changed);
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].step, 4);
  EXPECT_EQ(r.fills[0].instrument, 1);
  EXPECT_EQ(r.fills[0].basePrice, D("0.0245"));
  EXPECT_EQ(r.fills[0].price, D("0.024525"));
  EXPECT_EQ(r.orders[0].status, OrderStatus::Filled);
  EXPECT_EQ(pf.lots(1), 300);
  EXPECT_EQ(pf.lots(0), 0);
}

TEST(MarketOrders, ZeroLiquidityBudget) {
  // V = 9 лотов: floor(0.1 * 9) = 0 - заявка снимается, исполнений нет, портфель не тронут.
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(9))}, {});
  const ExecutionEngine engine({kA}, CostConfig::defaults(), false);
  Portfolio pf(D("1000000"), 1);
  Book r;
  r.orders.push_back(market(0, 0, Side::Buy, 5, 0));
  engine.processStep(1, t, r.orders, pf, r.fills, r.changed);
  EXPECT_TRUE(r.fills.empty());
  EXPECT_EQ(r.orders[0].status, OrderStatus::Cancelled);
  EXPECT_EQ(r.orders[0].reason, Reason::LiquidityLimit);
  EXPECT_EQ(r.orders[0].filledLots, 0);
  EXPECT_EQ(r.orders[0].closedStep, 1);
  ASSERT_EQ(r.changed.size(), 1u);
  EXPECT_EQ(pf.cash(), D("1000000"));
}

TEST(MarketOrders, PartialByLiquidity) {
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(2000))}, {});
  const ExecutionEngine engine({kA}, CostConfig::defaults(), false);
  Portfolio pf(D("1000000"), 1);
  Book r;
  r.orders.push_back(market(0, 0, Side::Buy, 300, 0));
  engine.processStep(1, t, r.orders, pf, r.fills, r.changed);
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].lots, 200);
  EXPECT_EQ(r.orders[0].status, OrderStatus::Cancelled);
  EXPECT_EQ(r.orders[0].filledLots, 200);
  EXPECT_EQ(r.orders[0].reason, Reason::LiquidityLimit);
  EXPECT_EQ(pf.lots(0), 200);
}

TEST(MarketOrders, LiquidityBudgetIsSharedByOrdersOfStep) {
  // Бюджет 200 лотов на два заявки: 150 + 50, вторая снимается с LIQUIDITY_LIMIT; порядок - по id.
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(2000))}, {});
  const ExecutionEngine engine({kA}, CostConfig::defaults(), false);
  Portfolio pf(D("1000000"), 1);
  Book r;
  r.orders.push_back(market(5, 0, Side::Buy, 100, 0));  // в векторе первая, но id больше
  r.orders.push_back(market(2, 0, Side::Buy, 150, 0));
  engine.processStep(1, t, r.orders, pf, r.fills, r.changed);
  ASSERT_EQ(r.fills.size(), 2u);
  EXPECT_EQ(r.fills[0].orderId, 2);
  EXPECT_EQ(r.fills[0].lots, 150);
  EXPECT_EQ(r.fills[1].orderId, 5);
  EXPECT_EQ(r.fills[1].lots, 50);
  EXPECT_EQ(r.orders[0].status, OrderStatus::Cancelled);
  EXPECT_EQ(r.orders[0].reason, Reason::LiquidityLimit);
  EXPECT_EQ(r.orders[1].status, OrderStatus::Filled);
  EXPECT_EQ(r.changed, (std::vector<std::int64_t>{2, 5}));
}

TEST(MarketOrders, ReducedByCash) {
  // Денег на 3 лота с издержками (≈ 3033.6 ₽), четвёртый уже не проходит.
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(50000))}, {});
  const CostConfig costs = CostConfig::defaults();
  const ExecutionEngine engine({kA}, costs, false);
  Portfolio pf(D("3500"), 1);
  Book r;
  r.orders.push_back(market(0, 0, Side::Buy, 10, 0));
  engine.processStep(1, t, r.orders, pf, r.fills, r.changed);
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].lots, 3);
  EXPECT_EQ(r.orders[0].status, OrderStatus::Cancelled);
  EXPECT_EQ(r.orders[0].filledLots, 3);
  EXPECT_EQ(r.orders[0].reason, Reason::InsufficientCash);
  EXPECT_GE(pf.cash(), D("0"));
  // Четвёртого лота не хватило бы (цена для 4 лотов пересчитывается).
  const Decimal p4 = core::fillPrice(Side::Buy, D("101"), 4, 50000, costs, kA.priceStep);
  const core::Amounts a4 = core::amounts(p4, 4, kA.lotSize, costs);
  EXPECT_GT(a4.value + a4.commission + a4.fee, D("3500"));
}

TEST(MarketOrders, NoCashForSingleLot) {
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(50000))}, {});
  const ExecutionEngine engine({kA}, CostConfig::defaults(), false);
  Portfolio pf(D("500"), 1);
  Book r;
  r.orders.push_back(market(0, 0, Side::Buy, 10, 0));
  engine.processStep(1, t, r.orders, pf, r.fills, r.changed);
  EXPECT_TRUE(r.fills.empty());
  EXPECT_EQ(r.orders[0].status, OrderStatus::Cancelled);
  EXPECT_EQ(r.orders[0].reason, Reason::InsufficientCash);
  EXPECT_EQ(pf.cash(), D("500"));
}

TEST(MarketOrders, SellReducedByPosition) {
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(50000))}, {});
  const ExecutionEngine engine({kA}, CostConfig::defaults(), false);
  Portfolio pf(D("1000000"), 1);
  pf.applyFill(0, Side::Buy, 5, D("100"), D("5000.00"), D("2.50"), D("0.50"), 0, 99);
  Book r;
  r.orders.push_back(market(0, 0, Side::Sell, 8, 0));
  engine.processStep(1, t, r.orders, pf, r.fills, r.changed);
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].lots, 5);
  EXPECT_EQ(r.fills[0].side, Side::Sell);
  EXPECT_LE(r.fills[0].price, D("101"));  // продажа - не выше open
  EXPECT_EQ(r.orders[0].status, OrderStatus::Cancelled);
  EXPECT_EQ(r.orders[0].reason, Reason::InsufficientPosition);
  EXPECT_EQ(pf.lots(0), 0);
}

TEST(MarketOrders, SellWithoutPositionIsCancelled) {
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(50000))}, {});
  const ExecutionEngine engine({kA}, CostConfig::defaults(), false);
  Portfolio pf(D("1000000"), 1);
  Book r;
  r.orders.push_back(market(0, 0, Side::Sell, 1, 0));
  engine.processStep(1, t, r.orders, pf, r.fills, r.changed);
  EXPECT_TRUE(r.fills.empty());
  EXPECT_EQ(r.orders[0].status, OrderStatus::Cancelled);
  EXPECT_EQ(r.orders[0].reason, Reason::InsufficientPosition);
}

TEST(MarketOrders, ClosedAndUnreachedConditionalOrdersAreUntouched) {
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(50000))}, {});
  const ExecutionEngine engine({kA}, CostConfig::defaults(), false);
  Portfolio pf(D("1000000"), 1);
  Book r;
  r.orders.push_back(market(0, 0, Side::Buy, 1, 0));
  r.orders[0].spec.type = OrderType::Limit;
  r.orders[0].spec.limitPrice = D("50");  // бар шага 1 (low 101) до уровня не доходит - лимитная остаётся активной
  r.orders.push_back(market(1, 0, Side::Buy, 1, 0));
  r.orders[1].status = OrderStatus::Cancelled;
  engine.processStep(1, t, r.orders, pf, r.fills, r.changed);
  EXPECT_TRUE(r.fills.empty());
  EXPECT_TRUE(r.changed.empty());
  EXPECT_EQ(r.orders[0].status, OrderStatus::Active);
  EXPECT_EQ(r.orders[1].status, OrderStatus::Cancelled);
}

TEST(MarketOrders, NoLiquidityLimitWhenShareUnset) {
  // Без лимита ликвидности (ФТ-24) заявка идёт целиком, проскальзывание всё равно считается от объёма.
  CostConfig costs = CostConfig::defaults();
  costs.liquidityShare.reset();
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(2000))}, {});
  const ExecutionEngine engine({kA}, costs, false);
  Portfolio pf(D("1000000"), 1);
  Book r;
  r.orders.push_back(market(0, 0, Side::Buy, 300, 0));
  engine.processStep(1, t, r.orders, pf, r.fills, r.changed);
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].lots, 300);
  EXPECT_EQ(r.orders[0].status, OrderStatus::Filled);
}

TEST(MarketOrders, ZeroVolumeBarWithoutLiquidityLimitStillFills) {
  // Без лимита ликвидности (ФТ-24) нулевой объём не снимает заявку: проскальзывания нет, остаётся спред.
  CostConfig costs = CostConfig::defaults();
  costs.liquidityShare.reset();
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(0))}, {});
  const ExecutionEngine engine({kA}, costs, false);
  Portfolio pf(D("1000000"), 1);
  Book r;
  r.orders.push_back(market(0, 0, Side::Buy, 100, 0));
  engine.processStep(1, t, r.orders, pf, r.fills, r.changed);
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].lots, 100);
  EXPECT_EQ(r.fills[0].price, D("101.06"));  // 101 * 1.0005 = 101.0505 -> шаг вверх
  EXPECT_EQ(r.orders[0].status, OrderStatus::Filled);
}

TEST(MarketOrders, ZeroVolumeBarWithLiquidityLimitIsCancelled) {
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(0))}, {});
  const ExecutionEngine engine({kA}, CostConfig::defaults(), false);
  Portfolio pf(D("1000000"), 1);
  Book r;
  r.orders.push_back(market(0, 0, Side::Buy, 1, 0));
  engine.processStep(1, t, r.orders, pf, r.fills, r.changed);
  EXPECT_TRUE(r.fills.empty());
  EXPECT_EQ(r.orders[0].reason, Reason::LiquidityLimit);
}

TEST(MarketOrders, SellFarAboveBarVolumeWithoutLimitDoesNotCrash) {
  // Продажа 100 лотов при V = 10 лотов без лимита ликвидности: цена зажата снизу шагом цены, прогон не падает.
  CostConfig costs = CostConfig::defaults();
  costs.liquidityShare.reset();
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(10))}, {});
  const ExecutionEngine engine({kA}, costs, false);
  Portfolio pf(D("1000000"), 1);
  pf.applyFill(0, Side::Buy, 100, D("100"), D("100000.00"), D("50.00"), D("10.00"), 0, 99);
  Book r;
  r.orders.push_back(market(0, 0, Side::Sell, 100, 0));
  engine.processStep(1, t, r.orders, pf, r.fills, r.changed);
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].price, D("0.01"));
  EXPECT_EQ(r.orders[0].status, OrderStatus::Filled);
  EXPECT_EQ(pf.lots(0), 0);
}

TEST(LiquidityBudget, IsSharedAndFloored) {
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(2005))}, {});
  const core::StepBar& bar = *t.barOf(0, 1);
  const CostConfig costs = CostConfig::defaults();
  core::LiquidityBudget budget(1);
  EXPECT_EQ(budget.remaining(0, bar, costs), 200);  // floor(0.1 * 2005)
  budget.consume(0, 150);
  EXPECT_EQ(budget.remaining(0, bar, costs), 50);  // второй вызов видит остаток, а не пересчитывает
  budget.consume(0, 50);
  EXPECT_EQ(budget.remaining(0, bar, costs), 0);
}

TEST(LiquidityBudget, UnlimitedWithoutShare) {
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(0))}, {});
  CostConfig costs = CostConfig::defaults();
  costs.liquidityShare.reset();
  core::LiquidityBudget budget(1);
  EXPECT_FALSE(budget.remaining(0, *t.barOf(0, 1), costs).has_value());
  budget.consume(0, 5);
  EXPECT_FALSE(budget.remaining(0, *t.barOf(0, 1), costs).has_value());
}

TEST(MarketOrders, ProcessNaiveRequiresNaiveEngine) {
  // Исключение из INV-2 (исполнение по close решения) доступно только за флагом naive.
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(50000))}, {});
  const ExecutionEngine engine({kA}, CostConfig::defaults(), false);
  Portfolio pf(D("1000000"), 1);
  Book r;
  r.orders.push_back(market(0, 0, Side::Buy, 1, 0));
  EXPECT_THROW(engine.processNaive(0, t, r.orders, pf, r.fills, r.changed), std::logic_error);
  EXPECT_TRUE(r.fills.empty());
  EXPECT_EQ(r.orders[0].status, OrderStatus::Active);
}

TEST(MarketOrders, NaiveFillsAtCloseWithoutCosts) {
  // Заявка шага 1 исполняется сразу по close шага 1 (101.5), без комиссии, сбора и спреда.
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(9))}, {});  // объём 9 лотов: лимит не мешает
  const ExecutionEngine engine({kA}, CostConfig::defaults(), true);
  EXPECT_TRUE(engine.naive());
  Portfolio pf(D("1000000"), 1);
  Book r;
  r.orders.push_back(market(0, 0, Side::Buy, 5, 1));
  engine.processNaive(1, t, r.orders, pf, r.fills, r.changed);
  ASSERT_EQ(r.fills.size(), 1u);
  const Fill& f = r.fills[0];
  EXPECT_EQ(f.step, 1);
  EXPECT_EQ(f.price, D("101.5"));
  EXPECT_EQ(f.basePrice, D("101.5"));
  EXPECT_EQ(f.value, D("5075.00"));
  EXPECT_TRUE(f.commission.isZero());
  EXPECT_TRUE(f.fee.isZero());
  EXPECT_TRUE(f.spreadCost.isZero());
  EXPECT_TRUE(f.slippageCost.isZero());
  EXPECT_EQ(r.orders[0].status, OrderStatus::Filled);
  EXPECT_EQ(pf.cash(), D("1000000") - D("5075.00"));
}

TEST(MarketOrders, NaiveUsesLastCloseOfOwnInstrument) {
  // У B нет бара на шаге 3: заявка B шага 3 исполняется по close последнего бара B (шаг 2).
  std::vector<B> b = {{0, "0.0240", "0.0240", 90000}, {1, "0.0242", "0.0242", 90000},
                      {2, "0.0244", "0.0244", 90000}, {4, "0.0245", "0.0245", 90000}};
  const Timeline t = Timeline::build({kA, kB}, {bars(kA, seriesA(50000)), bars(kB, b)}, {});
  const ExecutionEngine engine({kA, kB}, CostConfig::defaults(), true);
  Portfolio pf(D("1000000"), 2);
  Book r;
  r.orders.push_back(market(0, 1, Side::Buy, 10, 3));
  engine.processNaive(3, t, r.orders, pf, r.fills, r.changed);
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].price, D("0.0244"));
}

TEST(MarketOrders, NaiveIsLimitedByCashAndPosition) {
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(50000))}, {});
  const ExecutionEngine engine({kA}, CostConfig::defaults(), true);
  Portfolio pf(D("3050"), 1);  // лот по close 100.5 стоит 1005: хватает на 3 лота
  Book r;
  r.orders.push_back(market(0, 0, Side::Buy, 10, 0));
  engine.processNaive(0, t, r.orders, pf, r.fills, r.changed);
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].lots, 3);
  EXPECT_EQ(r.orders[0].reason, Reason::InsufficientCash);

  r.orders.push_back(market(1, 0, Side::Sell, 10, 0));
  engine.processNaive(0, t, r.orders, pf, r.fills, r.changed);
  ASSERT_EQ(r.fills.size(), 2u);
  EXPECT_EQ(r.fills[1].lots, 3);
  EXPECT_EQ(r.orders[1].reason, Reason::InsufficientPosition);
}

TEST(MarketOrders, NaiveDoesNotTouchOrdersPlacedLater) {
  const Timeline t = Timeline::build({kA}, {bars(kA, seriesA(50000))}, {});
  const ExecutionEngine engine({kA}, CostConfig::defaults(), true);
  Portfolio pf(D("1000000"), 1);
  Book r;
  r.orders.push_back(market(0, 0, Side::Buy, 1, 2));
  engine.processNaive(1, t, r.orders, pf, r.fills, r.changed);
  EXPECT_TRUE(r.fills.empty());
  EXPECT_EQ(r.orders[0].status, OrderStatus::Active);
}

}  // namespace
