#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
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
using core::TimeInForce;
using core::Timeline;

Decimal D(const char* text) { return Decimal::parse(text); }

constexpr std::int64_t kMinuteMs = 60'000;
constexpr std::int64_t kDayMs = 86'400'000;
constexpr std::int64_t kStart = 1'704'747'600'000 + 10 * 3'600'000;  // 2024-01-09 10:00 МСК

const Instrument kA{"A", "FIXA", 10, Decimal::parse("0.01")};
const Instrument kB{"B", "FIXB", 10, Decimal::parse("0.01")};

// Бар: торговый день (с 0), минута, open, high, low, close, объём в лотах.
struct B {
  int day;
  int t;
  const char* open;
  const char* high;
  const char* low;
  const char* close;
  std::int64_t volumeLots = 1000;
};

std::vector<Bar> bars(const Instrument& inst, const std::vector<B>& list) {
  std::vector<Bar> out;
  for (const B& b : list) {
    Bar bar;
    bar.tsOpenMs = kStart + b.day * kDayMs + b.t * kMinuteMs;
    bar.availableAtMs = bar.tsOpenMs + kMinuteMs;
    bar.open = D(b.open);
    bar.high = D(b.high);
    bar.low = D(b.low);
    bar.close = D(b.close);
    bar.volume = b.volumeLots * inst.lotSize;
    out.push_back(bar);
  }
  return out;
}

// Шаг 0 - бар подачи (close 100), шаг 1 - рабочий бар теста.
Timeline twoBars(const B& second) {
  return Timeline::build({kA}, {bars(kA, {B{0, 0, "100", "100", "100", "100"}, second})}, {});
}

Order makeOrder(std::int64_t id, Side side, OrderType type, std::int64_t lots, std::int64_t placedStep = 0) {
  Order o;
  o.id = id;
  o.spec.side = side;
  o.spec.type = type;
  o.spec.lots = lots;
  o.placedStep = placedStep;
  return o;
}

Order limitOrder(std::int64_t id, Side side, const char* limit, std::int64_t lots = 10) {
  Order o = makeOrder(id, side, OrderType::Limit, lots);
  o.spec.limitPrice = D(limit);
  return o;
}

Order stopOrder(std::int64_t id, Side side, const char* stop, std::int64_t lots = 10) {
  Order o = makeOrder(id, side, OrderType::Stop, lots);
  o.spec.stopPrice = D(stop);
  return o;
}

Order takeOrder(std::int64_t id, const char* price, std::int64_t lots = 10) {
  Order o = makeOrder(id, Side::Sell, OrderType::TakeProfit, lots);
  o.spec.limitPrice = D(price);
  return o;
}

struct Book {
  std::vector<Order> orders;
  std::vector<Fill> fills;
  std::vector<std::int64_t> changed;
};

// Портфель с позицией lots лотов бумаги A (покупка на шаге 0 по 100).
Portfolio holding(std::int64_t lots) {
  Portfolio pf(D("1000000"), 2);
  if (lots > 0) {
    pf.applyFill(0, Side::Buy, lots, D("100"), D("1000").times(lots), D("0"), D("0"), 0, 99);
  }
  return pf;
}

void step(const ExecutionEngine& engine, std::int64_t s, const Timeline& t, Book& book, Portfolio& pf) {
  engine.processStep(s, t, book.orders, pf, book.fills, book.changed);
}

ExecutionEngine engineA() { return ExecutionEngine({kA}, CostConfig::defaults(), false); }

// ---- Лимитные заявки -------------------------------------------------------------------------------------

TEST(Limit, BuyOpensBelowLimit) {
  const Timeline t = twoBars(B{0, 1, "99.00", "99.50", "98.50", "99.00"});
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  b.orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].basePrice, D("99"));
  EXPECT_EQ(b.fills[0].price, D("99.15"));  // raw 99.1485, не выше лимита
  EXPECT_EQ(b.fills[0].lots, 10);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Filled);
  EXPECT_EQ(b.orders[0].closedStep, 1);
}

TEST(Limit, BuyOpenSlightlyBelowLimitIsCappedByLimit) {
  // O = 99.95: O·1.0015 = 100.09 > L, цена не выше лимита.
  const Timeline t = twoBars(B{0, 1, "99.95", "100.50", "99.50", "100"});
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  b.orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].price, D("100.00"));
}

TEST(Limit, BuyTouchedNotCrossed) {
  const auto engine = engineA();
  {
    const Timeline t = twoBars(B{0, 1, "101", "102", "100.00", "101"});
    Portfolio pf = holding(0);
    Book b;
    b.orders.push_back(limitOrder(0, Side::Buy, "100.00"));
    step(engine, 1, t, b, pf);
    EXPECT_TRUE(b.fills.empty());
    EXPECT_EQ(b.orders[0].status, OrderStatus::Active);
  }
  {
    const Timeline t = twoBars(B{0, 1, "101", "102", "99.99", "101"});
    Portfolio pf = holding(0);
    Book b;
    b.orders.push_back(limitOrder(0, Side::Buy, "100.00"));
    step(engine, 1, t, b, pf);
    ASSERT_EQ(b.fills.size(), 1u);
    EXPECT_EQ(b.fills[0].price, D("100.00"));
    EXPECT_EQ(b.fills[0].basePrice, D("100.00"));
  }
}

TEST(Limit, FillAtLevelHasNoSpreadOrSlippageCost) {
  const Timeline t = twoBars(B{0, 1, "101", "102", "99.99", "101"});
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  b.orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].spreadCost, D("0"));
  EXPECT_EQ(b.fills[0].slippageCost, D("0"));
}

TEST(Limit, OpenFillCappedByLimitHasNoNegativeSlippage) {
  // O = 99.99: O·1.0015 = 100.14 > L, цена 100.00. Вся разница с базой (0.01·100 шт = 1.00) - спред, проскальзывание 0.
  const Timeline t = twoBars(B{0, 1, "99.99", "100.50", "99.50", "100"});
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  b.orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].price, D("100.00"));
  EXPECT_EQ(b.fills[0].basePrice, D("99.99"));
  EXPECT_EQ(b.fills[0].spreadCost, D("1"));
  EXPECT_EQ(b.fills[0].slippageCost, D("0"));
}

TEST(Limit, SellOpensAboveLimit) {
  const Timeline t = twoBars(B{0, 1, "101.00", "101.50", "100.50", "101"});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  b.orders.push_back(limitOrder(0, Side::Sell, "100.00"));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].price, D("100.84"));  // 101·0.9985 = 100.8485, вниз; выше лимита
  EXPECT_EQ(pf.lots(0), 0);
}

TEST(Limit, SellTouchedNotCrossed) {
  const auto engine = engineA();
  {
    const Timeline t = twoBars(B{0, 1, "99", "100.00", "98", "99"});
    Portfolio pf = holding(10);
    Book b;
    b.orders.push_back(limitOrder(0, Side::Sell, "100.00"));
    step(engine, 1, t, b, pf);
    EXPECT_TRUE(b.fills.empty());
  }
  {
    const Timeline t = twoBars(B{0, 1, "99", "100.01", "98", "99"});
    Portfolio pf = holding(10);
    Book b;
    b.orders.push_back(limitOrder(0, Side::Sell, "100.00"));
    step(engine, 1, t, b, pf);
    ASSERT_EQ(b.fills.size(), 1u);
    EXPECT_EQ(b.fills[0].price, D("100.00"));
  }
}

TEST(Limit, NotOnPlacementStep) {
  // INV-2: заявка шага 0 не исполняется на шаге 0, даже если бар шага 0 прошёл уровень.
  const Timeline t = twoBars(B{0, 1, "101", "102", "100.50", "101"});
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  b.orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(engine, 0, t, b, pf);
  EXPECT_TRUE(b.fills.empty());
  EXPECT_TRUE(b.changed.empty());
}

TEST(Limit, PartialFillKeepsRemainderActive) {
  // V = 50 -> бюджет floor(0.1·50) = 5 лотов; остаток 5 лотов остаётся активным и добирается на следующем баре.
  const Timeline t = Timeline::build({kA}, {bars(kA, {B{0, 0, "100", "100", "100", "100", 50},
                                                      B{0, 1, "99", "99.5", "98.5", "99", 50},
                                                      B{0, 2, "99", "99.5", "98.5", "99", 50}})},
                                     {});
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  b.orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].lots, 5);
  EXPECT_EQ(b.orders[0].status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(b.orders[0].filledLots, 5);
  EXPECT_EQ(b.orders[0].closedStep, -1);
  step(engine, 2, t, b, pf);
  ASSERT_EQ(b.fills.size(), 2u);
  EXPECT_EQ(b.fills[1].lots, 5);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Filled);
  EXPECT_EQ(pf.lots(0), 10);
}

TEST(Limit, ZeroLiquidityKeepsLimitActive) {
  // floor(0.1·5) = 0: рыночная снялась бы с LIQUIDITY_LIMIT, лимитная остаётся активной.
  const Timeline t = Timeline::build({kA}, {bars(kA, {B{0, 0, "100", "100", "100", "100", 5},
                                                      B{0, 1, "99", "99.5", "98.5", "99", 5}})},
                                     {});
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  b.orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(engine, 1, t, b, pf);
  EXPECT_TRUE(b.fills.empty());
  EXPECT_EQ(b.orders[0].status, OrderStatus::Active);
}

TEST(Limit, CashShortageCancelsRemainder) {
  const Timeline t = twoBars(B{0, 1, "99", "99.5", "98.5", "99"});
  const auto engine = engineA();
  Portfolio pf(D("5000"), 2);  // на 5 лотов по 99.15 с лотом 10 хватает, на 6 - нет
  Book b;
  b.orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].lots, 5);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Cancelled);
  EXPECT_EQ(b.orders[0].reason, Reason::InsufficientCash);
}

TEST(Limit, SharesBudgetWithMarketOrdersOfStep) {
  // V = 100 -> бюджет 10 лотов. Рыночная заявка (id больше) всё равно идёт первой и забирает бюджет целиком.
  const Timeline t = Timeline::build({kA}, {bars(kA, {B{0, 0, "100", "100", "100", "100", 100},
                                                      B{0, 1, "99", "99.5", "98.5", "99", 100}})},
                                     {});
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  b.orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  b.orders.push_back(makeOrder(1, Side::Buy, OrderType::Market, 10));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].orderId, 1);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Active);
}

TEST(Limit, WaitsForOwnInstrumentBar) {
  // У бумаги B на шаге 1 бара нет: её лимитная заявка ждёт, бар A её не исполняет.
  const Timeline t = Timeline::build(
      {kA, kB},
      {bars(kA, {B{0, 0, "100", "100", "100", "100"}, B{0, 1, "99", "99.5", "98.5", "99"}, B{0, 2, "99", "99", "98", "99"}}),
       bars(kB, {B{0, 0, "100", "100", "100", "100"}, B{0, 2, "99", "99.5", "98.5", "99"}})},
      {});
  const ExecutionEngine engine({kA, kB}, CostConfig::defaults(), false);
  Portfolio pf = holding(0);
  Book b;
  Order o = limitOrder(0, Side::Buy, "100.00");
  o.spec.instrument = 1;
  b.orders.push_back(o);
  step(engine, 1, t, b, pf);
  EXPECT_TRUE(b.fills.empty());
  EXPECT_EQ(b.orders[0].status, OrderStatus::Active);
  step(engine, 2, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].step, 2);
  EXPECT_EQ(b.fills[0].instrument, 1);
}

// ---- Стопы -----------------------------------------------------------------------------------------------

TEST(Stop, SellTriggeredIntraBar) {
  const Timeline t = twoBars(B{0, 1, "96", "97", "94", "95"});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  b.orders.push_back(stopOrder(0, Side::Sell, "95"));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].basePrice, D("95"));
  EXPECT_EQ(b.fills[0].price, D("94.85"));  // raw 94.8575
  EXPECT_FALSE(b.fills[0].stopAndTakeSameBar);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Filled);
}

TEST(Stop, SellGapDown) {
  const Timeline t = twoBars(B{0, 1, "90", "91", "89", "90"});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  b.orders.push_back(stopOrder(0, Side::Sell, "95"));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].basePrice, D("90"));
  EXPECT_EQ(b.fills[0].price, D("89.86"));  // raw 89.865
}

TEST(Stop, SellNotTriggeredAboveStop) {
  const Timeline t = twoBars(B{0, 1, "100", "101", "95.01", "100"});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  b.orders.push_back(stopOrder(0, Side::Sell, "95"));
  step(engine, 1, t, b, pf);
  EXPECT_TRUE(b.fills.empty());
  EXPECT_EQ(b.orders[0].status, OrderStatus::Active);
}

TEST(Stop, SellTouchTriggers) {
  // Для стопа касание уровня срабатывает (low <= S), в отличие от лимитной заявки.
  const Timeline t = twoBars(B{0, 1, "100", "101", "95.00", "100"});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  b.orders.push_back(stopOrder(0, Side::Sell, "95"));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].price, D("94.85"));
}

TEST(Stop, BuyTriggeredIntraBarAndGapUp) {
  const auto engine = engineA();
  {
    const Timeline t = twoBars(B{0, 1, "104", "106", "103", "105"});
    Portfolio pf = holding(0);
    Book b;
    b.orders.push_back(stopOrder(0, Side::Buy, "105"));
    step(engine, 1, t, b, pf);
    ASSERT_EQ(b.fills.size(), 1u);
    EXPECT_EQ(b.fills[0].basePrice, D("105"));
    EXPECT_EQ(b.fills[0].price, D("105.16"));  // 105·1.0015 = 105.1575, вверх
  }
  {
    const Timeline t = twoBars(B{0, 1, "108", "109", "107", "108"});
    Portfolio pf = holding(0);
    Book b;
    b.orders.push_back(stopOrder(0, Side::Buy, "105"));
    step(engine, 1, t, b, pf);
    ASSERT_EQ(b.fills.size(), 1u);
    EXPECT_EQ(b.fills[0].basePrice, D("108"));
    EXPECT_EQ(b.fills[0].price, D("108.17"));  // 108·1.0015 = 108.162, вверх
  }
}

TEST(Stop, RemainderBeyondLiquidityIsCancelled) {
  // V = 50 -> бюджет 5 лотов; остаток стопа снимается с LIQUIDITY_LIMIT и не переносится.
  const Timeline t = Timeline::build({kA}, {bars(kA, {B{0, 0, "100", "100", "100", "100", 50},
                                                      B{0, 1, "96", "97", "94", "95", 50},
                                                      B{0, 2, "96", "97", "94", "95", 50}})},
                                     {});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  b.orders.push_back(stopOrder(0, Side::Sell, "95"));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].lots, 5);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Cancelled);
  EXPECT_EQ(b.orders[0].reason, Reason::LiquidityLimit);
  step(engine, 2, t, b, pf);
  EXPECT_EQ(b.fills.size(), 1u);
}

TEST(Stop, NotOnPlacementStep) {
  const Timeline t = twoBars(B{0, 1, "96", "97", "94", "95"});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  b.orders.push_back(stopOrder(0, Side::Sell, "95"));
  step(engine, 0, t, b, pf);
  EXPECT_TRUE(b.fills.empty());
}

TEST(StopLimit, TriggeredBarDoesNotFillThenActsAsLimit) {
  // Бар 1 задел стоп 105 (high 106), но заявка не исполняется; с бара 2 - лимитная покупка по 106.
  const Timeline t = Timeline::build({kA}, {bars(kA, {B{0, 0, "100", "100", "100", "100"},
                                                      B{0, 1, "104", "106", "103", "105"},
                                                      B{0, 2, "107", "108", "106.50", "107"},
                                                      B{0, 3, "105", "107", "104", "105"}})},
                                     {});
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  Order o = makeOrder(0, Side::Buy, OrderType::StopLimit, 10);
  o.spec.stopPrice = D("105");
  o.spec.limitPrice = D("106");
  b.orders.push_back(o);
  step(engine, 1, t, b, pf);
  EXPECT_TRUE(b.fills.empty());
  EXPECT_TRUE(b.orders[0].triggered);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Active);
  step(engine, 2, t, b, pf);  // open 107 > 106, low 106.50 > 106: лимит не достигнут
  EXPECT_TRUE(b.fills.empty());
  step(engine, 3, t, b, pf);  // open 105 <= 106: исполнение от open
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].step, 3);
  EXPECT_EQ(b.fills[0].price, D("105.16"));  // 105·1.0015 = 105.1575, вверх; не выше лимита
}

TEST(StopLimit, UntriggeredStaysIdle) {
  const Timeline t = twoBars(B{0, 1, "100", "101", "99", "100"});
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  Order o = makeOrder(0, Side::Buy, OrderType::StopLimit, 10);
  o.spec.stopPrice = D("105");
  o.spec.limitPrice = D("106");
  b.orders.push_back(o);
  step(engine, 1, t, b, pf);
  EXPECT_FALSE(b.orders[0].triggered);
  EXPECT_TRUE(b.fills.empty());
}

// ---- Тейк-профит и трейлинг -----------------------------------------------------------------------------

TEST(TakeProfit, SellFillsAtPriceWhenCrossed) {
  const Timeline t = twoBars(B{0, 1, "100", "111", "99", "110"});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  b.orders.push_back(takeOrder(0, "110"));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].price, D("110.00"));
  EXPECT_FALSE(b.fills[0].stopAndTakeSameBar);
}

TEST(TakeProfit, TouchDoesNotFill) {
  const Timeline t = twoBars(B{0, 1, "100", "110", "99", "105"});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  b.orders.push_back(takeOrder(0, "110"));
  step(engine, 1, t, b, pf);
  EXPECT_TRUE(b.fills.empty());
}

TEST(Trailing, FollowsPeak) {
  // d = 5 %, close подачи 100 -> уровень 95.00. Бар 1: high 110, low 99 - не срабатывает, новый уровень 104.50.
  const Timeline t = Timeline::build({kA}, {bars(kA, {B{0, 0, "100", "100", "100", "100"},
                                                      B{0, 1, "100", "110", "99", "108"},
                                                      B{0, 2, "106", "107", "104", "105"}})},
                                     {});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  Order o = makeOrder(0, Side::Sell, OrderType::TrailingStop, 10);
  o.spec.trailingPercent = D("5");
  b.orders.push_back(o);
  step(engine, 1, t, b, pf);
  EXPECT_TRUE(b.fills.empty());
  EXPECT_EQ(*b.orders[0].trailingPeak, D("110"));
  EXPECT_EQ(*b.orders[0].trailingStopLevel, D("104.50"));
  step(engine, 2, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].basePrice, D("104.50"));
  EXPECT_EQ(b.fills[0].price, D("104.34"));  // raw 104.34325
  EXPECT_EQ(b.orders[0].status, OrderStatus::Filled);
}

TEST(Trailing, PeakUpdateMarksOrderChanged) {
  const Timeline t = Timeline::build({kA}, {bars(kA, {B{0, 0, "100", "100", "100", "100"},
                                                      B{0, 1, "100", "110", "99", "108"},
                                                      B{0, 2, "108", "109", "107", "108"}})},
                                     {});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  Order o = makeOrder(0, Side::Sell, OrderType::TrailingStop, 10);
  o.spec.trailingPercent = D("5");
  b.orders.push_back(o);
  step(engine, 1, t, b, pf);
  EXPECT_EQ(b.changed, (std::vector<std::int64_t>{0}));  // пик вырос 100 -> 110
  step(engine, 2, t, b, pf);
  EXPECT_EQ(b.changed.size(), 1u);  // пик не вырос (high 109 < 110): заявка не менялась
}

TEST(Trailing, RejectedAtSubmissionWithoutBarOfInstrument) {
  // У бумаги B первый бар на шаге 1: на шаге 0 цены для пика нет - InvalidParams по полю instrument.
  const Timeline t = Timeline::build(
      {kA, kB}, {bars(kA, {B{0, 0, "100", "100", "100", "100"}, B{0, 1, "100", "100", "100", "100"}}),
                 bars(kB, {B{0, 1, "100", "100", "100", "100"}})},
      {});
  const ExecutionEngine engine({kA, kB}, CostConfig::defaults(), false);
  Portfolio pf = holding(10);
  core::OrderSpec spec;
  spec.instrument = 1;
  spec.side = Side::Sell;
  spec.type = OrderType::TrailingStop;
  spec.lots = 1;
  spec.trailingPercent = D("5");
  const core::Validation v = engine.validate(spec, pf, t, 0);
  EXPECT_FALSE(v.ok);
  EXPECT_EQ(v.reason, Reason::InvalidParams);
  EXPECT_EQ(v.field, "instrument");
}

TEST(Trailing, LevelIsRoundedDownToPriceStep) {
  // Пик 100.07, d = 5 %: 95.0665 -> 95.06.
  const Timeline t = twoBars(B{0, 1, "100", "100.07", "99", "100"});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  Order o = makeOrder(0, Side::Sell, OrderType::TrailingStop, 10);
  o.spec.trailingPercent = D("5");
  b.orders.push_back(o);
  step(engine, 1, t, b, pf);
  EXPECT_EQ(*b.orders[0].trailingStopLevel, D("95.06"));
}

TEST(Trailing, GapDownExecutesAtOpen) {
  const Timeline t = Timeline::build({kA}, {bars(kA, {B{0, 0, "100", "100", "100", "100"},
                                                      B{0, 1, "90", "91", "89", "90"}})},
                                     {});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  Order o = makeOrder(0, Side::Sell, OrderType::TrailingStop, 10);
  o.spec.trailingPercent = D("5");
  b.orders.push_back(o);
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].basePrice, D("90"));
}

// ---- OCO и ФТ-9 -----------------------------------------------------------------------------------------

// Пара «стоп 95 + тейк 110» на 10 лотах; ноги связаны, если oco.
Book stopTakePair(bool oco) {
  Book b;
  b.orders.push_back(stopOrder(0, Side::Sell, "95"));
  b.orders.push_back(takeOrder(1, "110"));
  if (oco) {
    b.orders[0].ocoSiblingId = 1;
    b.orders[1].ocoSiblingId = 0;
  }
  return b;
}

TEST(Oco, FillCancelsSibling) {
  // Тейк достигнут, стоп нет: стоп снят с OCO_SIBLING_FILLED.
  const Timeline t = twoBars(B{0, 1, "100", "111", "99", "110"});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b = stopTakePair(true);
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].orderId, 1);
  EXPECT_EQ(b.orders[1].status, OrderStatus::Filled);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Cancelled);
  EXPECT_EQ(b.orders[0].reason, Reason::OcoSiblingFilled);
  EXPECT_EQ(b.orders[0].closedStep, 1);
  EXPECT_EQ(b.changed, (std::vector<std::int64_t>{1, 0}));
}

TEST(Oco, PartialFillOfOneLegCancelsSibling) {
  // V = 50 -> бюджет 5 лотов: тейк исполнен частично, стоп снят; остаток тейка остаётся активным.
  const Timeline t = Timeline::build({kA}, {bars(kA, {B{0, 0, "100", "100", "100", "100", 50},
                                                      B{0, 1, "100", "111", "99", "110", 50}})},
                                     {});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b = stopTakePair(true);
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].lots, 5);
  EXPECT_EQ(b.orders[1].status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Cancelled);
  EXPECT_EQ(b.orders[0].reason, Reason::OcoSiblingFilled);
}

TEST(Oco, CancelOneCancelsBoth) {
  Book b = stopTakePair(true);
  const bool ok = core::cancelOrder(b.orders, 1, Reason::CancelledByUser, 4, b.changed);
  EXPECT_TRUE(ok);
  for (const Order& o : b.orders) {
    EXPECT_EQ(o.status, OrderStatus::Cancelled);
    EXPECT_EQ(o.reason, Reason::CancelledByUser);
    EXPECT_EQ(o.closedStep, 4);
  }
  EXPECT_EQ(b.changed, (std::vector<std::int64_t>{1, 0}));
}

TEST(Oco, CancelInactiveOrderChangesNothing) {
  Book b = stopTakePair(true);
  b.orders[1].status = OrderStatus::Filled;
  EXPECT_FALSE(core::cancelOrder(b.orders, 1, Reason::CancelledByUser, 4, b.changed));
  EXPECT_FALSE(core::cancelOrder(b.orders, 77, Reason::CancelledByUser, 4, b.changed));
  EXPECT_EQ(b.orders[0].status, OrderStatus::Active);
  EXPECT_TRUE(b.changed.empty());
}

TEST(Oco, CancelWithoutSiblingTouchesOnlyOrder) {
  Book b = stopTakePair(false);
  EXPECT_TRUE(core::cancelOrder(b.orders, 0, Reason::CancelledByUser, 4, b.changed));
  EXPECT_EQ(b.orders[0].status, OrderStatus::Cancelled);
  EXPECT_EQ(b.orders[1].status, OrderStatus::Active);
}

TEST(StopBeforeTake, SameBar) {
  // ФТ-9: стоп 95 и тейк 110 на одном баре (high 111, low 94): исполняется стоп, сделка помечена, тейк снят.
  const Timeline t = twoBars(B{0, 1, "100", "111", "94", "100"});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b = stopTakePair(true);
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].orderId, 0);
  EXPECT_TRUE(b.fills[0].stopAndTakeSameBar);
  EXPECT_EQ(b.fills[0].basePrice, D("95"));
  EXPECT_EQ(b.orders[1].status, OrderStatus::Cancelled);
  EXPECT_EQ(b.orders[1].filledLots, 0);
  EXPECT_EQ(pf.lots(0), 0);
}

TEST(StopBeforeTake, SameBarWithoutOcoTakeGetsNoPosition) {
  // Без OCO тейк исполняется только на остаток позиции; остатка нет - снят без исполнения.
  const Timeline t = twoBars(B{0, 1, "100", "111", "94", "100"});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b = stopTakePair(false);
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_TRUE(b.fills[0].stopAndTakeSameBar);
  EXPECT_EQ(b.orders[1].status, OrderStatus::Cancelled);
  EXPECT_EQ(b.orders[1].reason, Reason::InsufficientPosition);
  EXPECT_EQ(b.orders[1].filledLots, 0);
}

TEST(StopBeforeTake, TakeOnRemainderOfPosition) {
  // Позиция 10 лотов: стоп на 4, тейк на 10 - тейк исполняется только на оставшиеся 6.
  const Timeline t = twoBars(B{0, 1, "100", "111", "94", "100"});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  b.orders.push_back(stopOrder(0, Side::Sell, "95", 4));
  b.orders.push_back(takeOrder(1, "110", 10));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 2u);
  EXPECT_TRUE(b.fills[0].stopAndTakeSameBar);
  EXPECT_EQ(b.fills[1].orderId, 1);
  EXPECT_EQ(b.fills[1].lots, 6);
  EXPECT_FALSE(b.fills[1].stopAndTakeSameBar);
  EXPECT_EQ(b.orders[1].status, OrderStatus::Cancelled);
  EXPECT_EQ(b.orders[1].reason, Reason::InsufficientPosition);
}

TEST(StopBeforeTake, NoFlagWhenTakeNotReached) {
  const Timeline t = twoBars(B{0, 1, "100", "109", "94", "100"});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b = stopTakePair(true);
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_FALSE(b.fills[0].stopAndTakeSameBar);
}

TEST(StopBeforeTake, TrailingStopCountsAsStop) {
  const Timeline t = twoBars(B{0, 1, "100", "111", "94", "100"});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  Order trailing = makeOrder(0, Side::Sell, OrderType::TrailingStop, 10);
  trailing.spec.trailingPercent = D("5");
  b.orders.push_back(trailing);
  b.orders.push_back(takeOrder(1, "110"));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_TRUE(b.fills[0].stopAndTakeSameBar);
}

// ---- Срок «день» -----------------------------------------------------------------------------------------

// День 1: шаги 0-2, день 2: шаги 3-5.
Timeline twoDays() {
  std::vector<B> list;
  for (int t = 0; t < 3; ++t) list.push_back(B{0, t, "101", "102", "100.5", "101"});
  for (int t = 0; t < 3; ++t) list.push_back(B{1, t, "101", "102", "100.5", "101"});
  return Timeline::build({kA}, {bars(kA, list)}, {});
}

TEST(Day, ExpiresEndOfFirstDay) {
  // Заявка, поданная на последнем шаге дня 1, действует весь день 2 и снимается после его последнего шага.
  const Timeline t = twoDays();
  ASSERT_TRUE(t.isLastStepOfDay(2));
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  Order o = limitOrder(0, Side::Buy, "50.00");
  o.spec.tif = TimeInForce::Day;
  o.placedStep = 2;
  b.orders.push_back(o);
  step(engine, 2, t, b, pf);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Active);  // последний шаг дня подачи - заявка ещё не действует
  step(engine, 3, t, b, pf);
  step(engine, 4, t, b, pf);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Active);
  step(engine, 5, t, b, pf);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Expired);
  EXPECT_EQ(b.orders[0].reason, Reason::DayExpired);
  EXPECT_EQ(b.orders[0].closedStep, 5);
  ASSERT_EQ(b.changed.size(), 1u);
  EXPECT_EQ(b.changed[0], 0);
}

TEST(Day, PlacedMidDayExpiresEndOfSameDay) {
  const Timeline t = twoDays();
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  Order o = limitOrder(0, Side::Buy, "50.00");
  o.spec.tif = TimeInForce::Day;
  o.placedStep = 0;
  b.orders.push_back(o);
  step(engine, 1, t, b, pf);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Active);
  step(engine, 2, t, b, pf);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Expired);
  EXPECT_EQ(b.orders[0].closedStep, 2);
}

TEST(Day, PartiallyFilledOrderExpiresRemainderOnce) {
  // Исполнена часть на последнем шаге дня, остаток снимается там же; в changed заявка одна.
  std::vector<B> list;
  list.push_back(B{0, 0, "101", "102", "100.5", "101", 50});
  list.push_back(B{0, 1, "99", "99.5", "98.5", "99", 50});
  const Timeline t = Timeline::build({kA}, {bars(kA, list)}, {});
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  Order o = limitOrder(0, Side::Buy, "100.00");
  o.spec.tif = TimeInForce::Day;
  b.orders.push_back(o);
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].lots, 5);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Expired);
  EXPECT_EQ(b.orders[0].filledLots, 5);
  EXPECT_EQ(b.changed.size(), 1u);
}

TEST(Day, GtcIsNeverExpired) {
  const Timeline t = twoDays();
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  b.orders.push_back(limitOrder(0, Side::Buy, "50.00"));
  for (int s = 1; s <= 5; ++s) step(engine, s, t, b, pf);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Active);
}

TEST(Day, OrderPlacedOnFinalStepIsLeftToSessionEnd) {
  const Timeline t = twoDays();
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  Order o = limitOrder(0, Side::Buy, "50.00");
  o.spec.tif = TimeInForce::Day;
  o.placedStep = 5;
  b.orders.push_back(o);
  step(engine, 5, t, b, pf);
  EXPECT_EQ(b.orders[0].status, OrderStatus::Active);
}

// ---- Порядок и детерминизм --------------------------------------------------------------------------------

TEST(ConditionalOrders, ProcessedInIdOrderWithinGroup) {
  // V = 100 -> бюджет 10 лотов. Две лимитные покупки по 10 лотов: исполняется меньший id, хотя в векторе он вторым.
  const Timeline t = Timeline::build({kA}, {bars(kA, {B{0, 0, "100", "100", "100", "100", 100},
                                                      B{0, 1, "99", "99.5", "98.5", "99", 100}})},
                                     {});
  const auto engine = engineA();
  Portfolio pf = holding(0);
  Book b;
  b.orders.push_back(limitOrder(7, Side::Buy, "100.00"));
  b.orders.push_back(limitOrder(3, Side::Buy, "100.00"));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].orderId, 3);
}

TEST(ConditionalOrders, StopsRunBeforeLimits) {
  // V = 100 -> бюджет 10 лотов. Лимитная продажа (id 0) и стоп (id 1): стоп идёт раньше и забирает бюджет.
  const Timeline t = Timeline::build({kA}, {bars(kA, {B{0, 0, "100", "100", "100", "100", 100},
                                                      B{0, 1, "96", "101", "94", "95", 100}})},
                                     {});
  const auto engine = engineA();
  Portfolio pf = holding(10);
  Book b;
  b.orders.push_back(limitOrder(0, Side::Sell, "100.50"));
  b.orders.push_back(stopOrder(1, Side::Sell, "95"));
  step(engine, 1, t, b, pf);
  ASSERT_EQ(b.fills.size(), 1u);
  EXPECT_EQ(b.fills[0].orderId, 1);
  EXPECT_TRUE(b.fills[0].stopAndTakeSameBar);  // лимитная продажа 100.50 тоже была бы задета (high 101)
}

}  // namespace
