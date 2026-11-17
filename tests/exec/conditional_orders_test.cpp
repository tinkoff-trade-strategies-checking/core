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

Decimal d(const char *Text) { return Decimal::parse(Text); }

constexpr std::int64_t kMinuteMs = 60'000;
constexpr std::int64_t kDayMs = 86'400'000;
constexpr std::int64_t kStart =
    1'704'747'600'000 + 10 * 3'600'000; // 2024-01-09 10:00 МСК

const Instrument kA{"A", "FIXA", 10, Decimal::parse("0.01")};
const Instrument kB{"B", "FIXB", 10, Decimal::parse("0.01")};

// Бар: торговый день (с 0), минута, open, high, low, close, объём в лотах.
struct B {
  int Day;
  int T;
  const char *Open;
  const char *High;
  const char *Low;
  const char *Close;
  std::int64_t VolumeLots = 1000;
};

std::vector<Bar> bars(const Instrument &Inst, const std::vector<B> &List) {
  std::vector<Bar> Out;
  for (const B &B : List) {
    Bar Bar;
    Bar.TsOpenMs = kStart + B.Day * kDayMs + B.T * kMinuteMs;
    Bar.AvailableAtMs = Bar.TsOpenMs + kMinuteMs;
    Bar.Open = d(B.Open);
    Bar.High = d(B.High);
    Bar.Low = d(B.Low);
    Bar.Close = d(B.Close);
    Bar.Volume = B.VolumeLots * Inst.LotSize;
    Out.push_back(Bar);
  }
  return Out;
}

// Шаг 0 - бар подачи (close 100), шаг 1 - рабочий бар теста.
Timeline twoBars(const B &Second) {
  return Timeline::build(
      {kA}, {bars(kA, {B{0, 0, "100", "100", "100", "100"}, Second})}, {});
}

Order makeOrder(std::int64_t Id, Side Side, OrderType Type, std::int64_t Lots,
                std::int64_t PlacedStep = 0) {
  Order O;
  O.Id = Id;
  O.Spec.OrderSide = Side;
  O.Spec.Type = Type;
  O.Spec.Lots = Lots;
  O.PlacedStep = PlacedStep;
  return O;
}

Order limitOrder(std::int64_t Id, Side Side, const char *Limit,
                 std::int64_t Lots = 10) {
  Order O = makeOrder(Id, Side, OrderType::Limit, Lots);
  O.Spec.LimitPrice = d(Limit);
  return O;
}

Order stopOrder(std::int64_t Id, Side Side, const char *Stop,
                std::int64_t Lots = 10) {
  Order O = makeOrder(Id, Side, OrderType::Stop, Lots);
  O.Spec.StopPrice = d(Stop);
  return O;
}

Order takeOrder(std::int64_t Id, const char *Price, std::int64_t Lots = 10) {
  Order O = makeOrder(Id, Side::Sell, OrderType::TakeProfit, Lots);
  O.Spec.LimitPrice = d(Price);
  return O;
}

struct Book {
  std::vector<Order> Orders;
  std::vector<Fill> Fills;
  std::vector<std::int64_t> Changed;
};

// Портфель с позицией lots лотов бумаги A (покупка на шаге 0 по 100).
Portfolio holding(std::int64_t Lots) {
  Portfolio Pf(d("1000000"), 2);
  if (Lots > 0) {
    Pf.applyFill(0, Side::Buy, Lots, d("100"), d("1000").times(Lots), d("0"),
                 d("0"), 0, 99);
  }
  return Pf;
}

void step(const ExecutionEngine &Engine, std::int64_t S, const Timeline &T,
          Book &Book, Portfolio &Pf) {
  Engine.processStep(S, T, Book.Orders, Pf, Book.Fills, Book.Changed);
}

ExecutionEngine engineA() {
  return ExecutionEngine({kA}, CostConfig::defaults());
}

// ---- Лимитные заявки
// -------------------------------------------------------------------------------------

TEST(Limit, BuyOpensBelowLimit) {
  const Timeline T = twoBars(B{0, 1, "99.00", "99.50", "98.50", "99.00"});
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  B.Orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].BasePrice, d("99"));
  EXPECT_EQ(B.Fills[0].Price, d("99.15")); // raw 99.1485, не выше лимита
  EXPECT_EQ(B.Fills[0].Lots, 10);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Filled);
  EXPECT_EQ(B.Orders[0].ClosedStep, 1);
}

TEST(Limit, BuyOpenSlightlyBelowLimitIsCappedByLimit) {
  // O = 99.95: O·1.0015 = 100.09 > L, цена не выше лимита.
  const Timeline T = twoBars(B{0, 1, "99.95", "100.50", "99.50", "100"});
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  B.Orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].Price, d("100.00"));
}

TEST(Limit, BuyTouchedNotCrossed) {
  const auto Engine = engineA();
  {
    const Timeline T = twoBars(B{0, 1, "101", "102", "100.00", "101"});
    Portfolio Pf = holding(0);
    Book B;
    B.Orders.push_back(limitOrder(0, Side::Buy, "100.00"));
    step(Engine, 1, T, B, Pf);
    EXPECT_TRUE(B.Fills.empty());
    EXPECT_EQ(B.Orders[0].Status, OrderStatus::Active);
  }
  {
    const Timeline T = twoBars(B{0, 1, "101", "102", "99.99", "101"});
    Portfolio Pf = holding(0);
    Book B;
    B.Orders.push_back(limitOrder(0, Side::Buy, "100.00"));
    step(Engine, 1, T, B, Pf);
    ASSERT_EQ(B.Fills.size(), 1u);
    EXPECT_EQ(B.Fills[0].Price, d("100.00"));
    EXPECT_EQ(B.Fills[0].BasePrice, d("100.00"));
  }
}

TEST(Limit, FillAtLevelHasNoSpreadOrSlippageCost) {
  const Timeline T = twoBars(B{0, 1, "101", "102", "99.99", "101"});
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  B.Orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].SpreadCost, d("0"));
  EXPECT_EQ(B.Fills[0].SlippageCost, d("0"));
}

TEST(Limit, OpenFillCappedByLimitHasNoNegativeSlippage) {
  // O = 99.99: O·1.0015 = 100.14 > L, цена 100.00. Вся разница с базой
  // (0.01·100 шт = 1.00) - спред, проскальзывание 0.
  const Timeline T = twoBars(B{0, 1, "99.99", "100.50", "99.50", "100"});
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  B.Orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].Price, d("100.00"));
  EXPECT_EQ(B.Fills[0].BasePrice, d("99.99"));
  EXPECT_EQ(B.Fills[0].SpreadCost, d("1"));
  EXPECT_EQ(B.Fills[0].SlippageCost, d("0"));
}

TEST(Limit, SellOpensAboveLimit) {
  const Timeline T = twoBars(B{0, 1, "101.00", "101.50", "100.50", "101"});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  B.Orders.push_back(limitOrder(0, Side::Sell, "100.00"));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].Price,
            d("100.84")); // 101·0.9985 = 100.8485, вниз; выше лимита
  EXPECT_EQ(Pf.lots(0), 0);
}

TEST(Limit, SellTouchedNotCrossed) {
  const auto Engine = engineA();
  {
    const Timeline T = twoBars(B{0, 1, "99", "100.00", "98", "99"});
    Portfolio Pf = holding(10);
    Book B;
    B.Orders.push_back(limitOrder(0, Side::Sell, "100.00"));
    step(Engine, 1, T, B, Pf);
    EXPECT_TRUE(B.Fills.empty());
  }
  {
    const Timeline T = twoBars(B{0, 1, "99", "100.01", "98", "99"});
    Portfolio Pf = holding(10);
    Book B;
    B.Orders.push_back(limitOrder(0, Side::Sell, "100.00"));
    step(Engine, 1, T, B, Pf);
    ASSERT_EQ(B.Fills.size(), 1u);
    EXPECT_EQ(B.Fills[0].Price, d("100.00"));
  }
}

TEST(Limit, NotOnPlacementStep) {
  // INV-2: заявка шага 0 не исполняется на шаге 0, даже если бар шага 0 прошёл
  // уровень.
  const Timeline T = twoBars(B{0, 1, "101", "102", "100.50", "101"});
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  B.Orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(Engine, 0, T, B, Pf);
  EXPECT_TRUE(B.Fills.empty());
  EXPECT_TRUE(B.Changed.empty());
}

TEST(Limit, PartialFillKeepsRemainderActive) {
  // V = 50 -> бюджет floor(0.1·50) = 5 лотов; остаток 5 лотов остаётся активным
  // и добирается на следующем баре.
  const Timeline T =
      Timeline::build({kA},
                      {bars(kA, {B{0, 0, "100", "100", "100", "100", 50},
                                 B{0, 1, "99", "99.5", "98.5", "99", 50},
                                 B{0, 2, "99", "99.5", "98.5", "99", 50}})},
                      {});
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  B.Orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].Lots, 5);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(B.Orders[0].FilledLots, 5);
  EXPECT_EQ(B.Orders[0].ClosedStep, -1);
  step(Engine, 2, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 2u);
  EXPECT_EQ(B.Fills[1].Lots, 5);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Filled);
  EXPECT_EQ(Pf.lots(0), 10);
}

TEST(Limit, ZeroLiquidityKeepsLimitActive) {
  // floor(0.1·5) = 0: рыночная снялась бы с LIQUIDITY_LIMIT, лимитная остаётся
  // активной.
  const Timeline T =
      Timeline::build({kA},
                      {bars(kA, {B{0, 0, "100", "100", "100", "100", 5},
                                 B{0, 1, "99", "99.5", "98.5", "99", 5}})},
                      {});
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  B.Orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(Engine, 1, T, B, Pf);
  EXPECT_TRUE(B.Fills.empty());
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Active);
}

TEST(Limit, CashShortageCancelsRemainder) {
  const Timeline T = twoBars(B{0, 1, "99", "99.5", "98.5", "99"});
  const auto Engine = engineA();
  Portfolio Pf(d("5000"),
               2); // на 5 лотов по 99.15 с лотом 10 хватает, на 6 - нет
  Book B;
  B.Orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].Lots, 5);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(B.Orders[0].ReasonCode, Reason::InsufficientCash);
}

TEST(Limit, SharesBudgetWithMarketOrdersOfStep) {
  // V = 100 -> бюджет 10 лотов. Рыночная заявка (id больше) всё равно идёт
  // первой и забирает бюджет целиком.
  const Timeline T =
      Timeline::build({kA},
                      {bars(kA, {B{0, 0, "100", "100", "100", "100", 100},
                                 B{0, 1, "99", "99.5", "98.5", "99", 100}})},
                      {});
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  B.Orders.push_back(limitOrder(0, Side::Buy, "100.00"));
  B.Orders.push_back(makeOrder(1, Side::Buy, OrderType::Market, 10));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].OrderId, 1);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Active);
}

TEST(Limit, WaitsForOwnInstrumentBar) {
  // У бумаги B на шаге 1 бара нет: её лимитная заявка ждёт, бар A её не
  // исполняет.
  const Timeline T =
      Timeline::build({kA, kB},
                      {bars(kA, {B{0, 0, "100", "100", "100", "100"},
                                 B{0, 1, "99", "99.5", "98.5", "99"},
                                 B{0, 2, "99", "99", "98", "99"}}),
                       bars(kB, {B{0, 0, "100", "100", "100", "100"},
                                 B{0, 2, "99", "99.5", "98.5", "99"}})},
                      {});
  const ExecutionEngine Engine({kA, kB}, CostConfig::defaults());
  Portfolio Pf = holding(0);
  Book B;
  Order O = limitOrder(0, Side::Buy, "100.00");
  O.Spec.Instrument = 1;
  B.Orders.push_back(O);
  step(Engine, 1, T, B, Pf);
  EXPECT_TRUE(B.Fills.empty());
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Active);
  step(Engine, 2, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].Step, 2);
  EXPECT_EQ(B.Fills[0].Instrument, 1);
}

// ---- Стопы
// -----------------------------------------------------------------------------------------------

TEST(Stop, SellTriggeredIntraBar) {
  const Timeline T = twoBars(B{0, 1, "96", "97", "94", "95"});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  B.Orders.push_back(stopOrder(0, Side::Sell, "95"));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].BasePrice, d("95"));
  EXPECT_EQ(B.Fills[0].Price, d("94.85")); // raw 94.8575
  EXPECT_FALSE(B.Fills[0].StopAndTakeSameBar);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Filled);
}

TEST(Stop, SellGapDown) {
  const Timeline T = twoBars(B{0, 1, "90", "91", "89", "90"});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  B.Orders.push_back(stopOrder(0, Side::Sell, "95"));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].BasePrice, d("90"));
  EXPECT_EQ(B.Fills[0].Price, d("89.86")); // raw 89.865
}

TEST(Stop, SellNotTriggeredAboveStop) {
  const Timeline T = twoBars(B{0, 1, "100", "101", "95.01", "100"});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  B.Orders.push_back(stopOrder(0, Side::Sell, "95"));
  step(Engine, 1, T, B, Pf);
  EXPECT_TRUE(B.Fills.empty());
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Active);
}

TEST(Stop, SellTouchTriggers) {
  // Для стопа касание уровня срабатывает (low <= S), в отличие от лимитной
  // заявки.
  const Timeline T = twoBars(B{0, 1, "100", "101", "95.00", "100"});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  B.Orders.push_back(stopOrder(0, Side::Sell, "95"));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].Price, d("94.85"));
}

TEST(Stop, BuyTriggeredIntraBarAndGapUp) {
  const auto Engine = engineA();
  {
    const Timeline T = twoBars(B{0, 1, "104", "106", "103", "105"});
    Portfolio Pf = holding(0);
    Book B;
    B.Orders.push_back(stopOrder(0, Side::Buy, "105"));
    step(Engine, 1, T, B, Pf);
    ASSERT_EQ(B.Fills.size(), 1u);
    EXPECT_EQ(B.Fills[0].BasePrice, d("105"));
    EXPECT_EQ(B.Fills[0].Price, d("105.16")); // 105·1.0015 = 105.1575, вверх
  }
  {
    const Timeline T = twoBars(B{0, 1, "108", "109", "107", "108"});
    Portfolio Pf = holding(0);
    Book B;
    B.Orders.push_back(stopOrder(0, Side::Buy, "105"));
    step(Engine, 1, T, B, Pf);
    ASSERT_EQ(B.Fills.size(), 1u);
    EXPECT_EQ(B.Fills[0].BasePrice, d("108"));
    EXPECT_EQ(B.Fills[0].Price, d("108.17")); // 108·1.0015 = 108.162, вверх
  }
}

TEST(Stop, RemainderBeyondLiquidityIsCancelled) {
  // V = 50 -> бюджет 5 лотов; остаток стопа снимается с LIQUIDITY_LIMIT и не
  // переносится.
  const Timeline T =
      Timeline::build({kA},
                      {bars(kA, {B{0, 0, "100", "100", "100", "100", 50},
                                 B{0, 1, "96", "97", "94", "95", 50},
                                 B{0, 2, "96", "97", "94", "95", 50}})},
                      {});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  B.Orders.push_back(stopOrder(0, Side::Sell, "95"));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].Lots, 5);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(B.Orders[0].ReasonCode, Reason::LiquidityLimit);
  step(Engine, 2, T, B, Pf);
  EXPECT_EQ(B.Fills.size(), 1u);
}

TEST(Stop, NotOnPlacementStep) {
  const Timeline T = twoBars(B{0, 1, "96", "97", "94", "95"});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  B.Orders.push_back(stopOrder(0, Side::Sell, "95"));
  step(Engine, 0, T, B, Pf);
  EXPECT_TRUE(B.Fills.empty());
}

TEST(StopLimit, TriggeredBarDoesNotFillThenActsAsLimit) {
  // Бар 1 задел стоп 105 (high 106), но заявка не исполняется; с бара 2 -
  // лимитная покупка по 106.
  const Timeline T =
      Timeline::build({kA},
                      {bars(kA, {B{0, 0, "100", "100", "100", "100"},
                                 B{0, 1, "104", "106", "103", "105"},
                                 B{0, 2, "107", "108", "106.50", "107"},
                                 B{0, 3, "105", "107", "104", "105"}})},
                      {});
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  Order O = makeOrder(0, Side::Buy, OrderType::StopLimit, 10);
  O.Spec.StopPrice = d("105");
  O.Spec.LimitPrice = d("106");
  B.Orders.push_back(O);
  step(Engine, 1, T, B, Pf);
  EXPECT_TRUE(B.Fills.empty());
  EXPECT_TRUE(B.Orders[0].Triggered);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Active);
  step(Engine, 2, T, B,
       Pf); // open 107 > 106, low 106.50 > 106: лимит не достигнут
  EXPECT_TRUE(B.Fills.empty());
  step(Engine, 3, T, B, Pf); // open 105 <= 106: исполнение от open
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].Step, 3);
  EXPECT_EQ(B.Fills[0].Price,
            d("105.16")); // 105·1.0015 = 105.1575, вверх; не выше лимита
}

TEST(StopLimit, UntriggeredStaysIdle) {
  const Timeline T = twoBars(B{0, 1, "100", "101", "99", "100"});
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  Order O = makeOrder(0, Side::Buy, OrderType::StopLimit, 10);
  O.Spec.StopPrice = d("105");
  O.Spec.LimitPrice = d("106");
  B.Orders.push_back(O);
  step(Engine, 1, T, B, Pf);
  EXPECT_FALSE(B.Orders[0].Triggered);
  EXPECT_TRUE(B.Fills.empty());
}

// ---- Тейк-профит и трейлинг
// -----------------------------------------------------------------------------

TEST(TakeProfit, SellFillsAtPriceWhenCrossed) {
  const Timeline T = twoBars(B{0, 1, "100", "111", "99", "110"});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  B.Orders.push_back(takeOrder(0, "110"));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].Price, d("110.00"));
  EXPECT_FALSE(B.Fills[0].StopAndTakeSameBar);
}

TEST(TakeProfit, TouchDoesNotFill) {
  const Timeline T = twoBars(B{0, 1, "100", "110", "99", "105"});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  B.Orders.push_back(takeOrder(0, "110"));
  step(Engine, 1, T, B, Pf);
  EXPECT_TRUE(B.Fills.empty());
}

TEST(Trailing, FollowsPeak) {
  // d = 5 %, close подачи 100 -> уровень 95.00. Бар 1: high 110, low 99 - не
  // срабатывает, новый уровень 104.50.
  const Timeline T =
      Timeline::build({kA},
                      {bars(kA, {B{0, 0, "100", "100", "100", "100"},
                                 B{0, 1, "100", "110", "99", "108"},
                                 B{0, 2, "106", "107", "104", "105"}})},
                      {});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  Order O = makeOrder(0, Side::Sell, OrderType::TrailingStop, 10);
  O.Spec.TrailingPercent = d("5");
  B.Orders.push_back(O);
  step(Engine, 1, T, B, Pf);
  EXPECT_TRUE(B.Fills.empty());
  EXPECT_EQ(*B.Orders[0].TrailingPeak, d("110"));
  EXPECT_EQ(*B.Orders[0].TrailingStopLevel, d("104.50"));
  step(Engine, 2, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].BasePrice, d("104.50"));
  EXPECT_EQ(B.Fills[0].Price, d("104.34")); // raw 104.34325
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Filled);
}

TEST(Trailing, PeakUpdateMarksOrderChanged) {
  const Timeline T =
      Timeline::build({kA},
                      {bars(kA, {B{0, 0, "100", "100", "100", "100"},
                                 B{0, 1, "100", "110", "99", "108"},
                                 B{0, 2, "108", "109", "107", "108"}})},
                      {});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  Order O = makeOrder(0, Side::Sell, OrderType::TrailingStop, 10);
  O.Spec.TrailingPercent = d("5");
  B.Orders.push_back(O);
  step(Engine, 1, T, B, Pf);
  EXPECT_EQ(B.Changed, (std::vector<std::int64_t>{0})); // пик вырос 100 -> 110
  step(Engine, 2, T, B, Pf);
  EXPECT_EQ(B.Changed.size(),
            1u); // пик не вырос (high 109 < 110): заявка не менялась
}

TEST(Trailing, RejectedAtSubmissionWithoutBarOfInstrument) {
  // У бумаги B первый бар на шаге 1: на шаге 0 цены для пика нет -
  // InvalidParams по полю instrument.
  const Timeline T =
      Timeline::build({kA, kB},
                      {bars(kA, {B{0, 0, "100", "100", "100", "100"},
                                 B{0, 1, "100", "100", "100", "100"}}),
                       bars(kB, {B{0, 1, "100", "100", "100", "100"}})},
                      {});
  const ExecutionEngine Engine({kA, kB}, CostConfig::defaults());
  Portfolio Pf = holding(10);
  core::OrderSpec Spec;
  Spec.Instrument = 1;
  Spec.OrderSide = Side::Sell;
  Spec.Type = OrderType::TrailingStop;
  Spec.Lots = 1;
  Spec.TrailingPercent = d("5");
  const core::Validation V = Engine.validate(Spec, Pf, T, 0);
  EXPECT_FALSE(V.Ok);
  EXPECT_EQ(V.ReasonCode, Reason::InvalidParams);
  EXPECT_EQ(V.Field, "instrument");
}

TEST(Trailing, LevelIsRoundedDownToPriceStep) {
  // Пик 100.07, d = 5 %: 95.0665 -> 95.06.
  const Timeline T = twoBars(B{0, 1, "100", "100.07", "99", "100"});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  Order O = makeOrder(0, Side::Sell, OrderType::TrailingStop, 10);
  O.Spec.TrailingPercent = d("5");
  B.Orders.push_back(O);
  step(Engine, 1, T, B, Pf);
  EXPECT_EQ(*B.Orders[0].TrailingStopLevel, d("95.06"));
}

TEST(Trailing, GapDownExecutesAtOpen) {
  const Timeline T =
      Timeline::build({kA},
                      {bars(kA, {B{0, 0, "100", "100", "100", "100"},
                                 B{0, 1, "90", "91", "89", "90"}})},
                      {});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  Order O = makeOrder(0, Side::Sell, OrderType::TrailingStop, 10);
  O.Spec.TrailingPercent = d("5");
  B.Orders.push_back(O);
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].BasePrice, d("90"));
}

// ---- OCO и ФТ-9
// -----------------------------------------------------------------------------------------

// Пара «стоп 95 + тейк 110» на 10 лотах; ноги связаны, если oco.
Book stopTakePair(bool Oco) {
  Book B;
  B.Orders.push_back(stopOrder(0, Side::Sell, "95"));
  B.Orders.push_back(takeOrder(1, "110"));
  if (Oco) {
    B.Orders[0].OcoSiblingId = 1;
    B.Orders[1].OcoSiblingId = 0;
  }
  return B;
}

TEST(Oco, FillCancelsSibling) {
  // Тейк достигнут, стоп нет: стоп снят с OCO_SIBLING_FILLED.
  const Timeline T = twoBars(B{0, 1, "100", "111", "99", "110"});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B = stopTakePair(true);
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].OrderId, 1);
  EXPECT_EQ(B.Orders[1].Status, OrderStatus::Filled);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(B.Orders[0].ReasonCode, Reason::OcoSiblingFilled);
  EXPECT_EQ(B.Orders[0].ClosedStep, 1);
  EXPECT_EQ(B.Changed, (std::vector<std::int64_t>{1, 0}));
}

TEST(Oco, PartialFillOfOneLegCancelsSibling) {
  // V = 50 -> бюджет 5 лотов: тейк исполнен частично, стоп снят; остаток тейка
  // остаётся активным.
  const Timeline T =
      Timeline::build({kA},
                      {bars(kA, {B{0, 0, "100", "100", "100", "100", 50},
                                 B{0, 1, "100", "111", "99", "110", 50}})},
                      {});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B = stopTakePair(true);
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].Lots, 5);
  EXPECT_EQ(B.Orders[1].Status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(B.Orders[0].ReasonCode, Reason::OcoSiblingFilled);
}

TEST(Oco, CancelOneCancelsBoth) {
  Book B = stopTakePair(true);
  const bool Ok =
      core::cancelOrder(B.Orders, 1, Reason::CancelledByUser, 4, B.Changed);
  EXPECT_TRUE(Ok);
  for (const Order &O : B.Orders) {
    EXPECT_EQ(O.Status, OrderStatus::Cancelled);
    EXPECT_EQ(O.ReasonCode, Reason::CancelledByUser);
    EXPECT_EQ(O.ClosedStep, 4);
  }
  EXPECT_EQ(B.Changed, (std::vector<std::int64_t>{1, 0}));
}

TEST(Oco, CancelInactiveOrderChangesNothing) {
  Book B = stopTakePair(true);
  B.Orders[1].Status = OrderStatus::Filled;
  EXPECT_FALSE(
      core::cancelOrder(B.Orders, 1, Reason::CancelledByUser, 4, B.Changed));
  EXPECT_FALSE(
      core::cancelOrder(B.Orders, 77, Reason::CancelledByUser, 4, B.Changed));
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Active);
  EXPECT_TRUE(B.Changed.empty());
}

TEST(Oco, CancelWithoutSiblingTouchesOnlyOrder) {
  Book B = stopTakePair(false);
  EXPECT_TRUE(
      core::cancelOrder(B.Orders, 0, Reason::CancelledByUser, 4, B.Changed));
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(B.Orders[1].Status, OrderStatus::Active);
}

TEST(StopBeforeTake, SameBar) {
  // ФТ-9: стоп 95 и тейк 110 на одном баре (high 111, low 94): исполняется
  // стоп, сделка помечена, тейк снят.
  const Timeline T = twoBars(B{0, 1, "100", "111", "94", "100"});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B = stopTakePair(true);
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].OrderId, 0);
  EXPECT_TRUE(B.Fills[0].StopAndTakeSameBar);
  EXPECT_EQ(B.Fills[0].BasePrice, d("95"));
  EXPECT_EQ(B.Orders[1].Status, OrderStatus::Cancelled);
  EXPECT_EQ(B.Orders[1].FilledLots, 0);
  EXPECT_EQ(Pf.lots(0), 0);
}

TEST(StopBeforeTake, SameBarWithoutOcoTakeGetsNoPosition) {
  // Без OCO тейк исполняется только на остаток позиции; остатка нет - снят без
  // исполнения.
  const Timeline T = twoBars(B{0, 1, "100", "111", "94", "100"});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B = stopTakePair(false);
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_TRUE(B.Fills[0].StopAndTakeSameBar);
  EXPECT_EQ(B.Orders[1].Status, OrderStatus::Cancelled);
  EXPECT_EQ(B.Orders[1].ReasonCode, Reason::InsufficientPosition);
  EXPECT_EQ(B.Orders[1].FilledLots, 0);
}

TEST(StopBeforeTake, TakeOnRemainderOfPosition) {
  // Позиция 10 лотов: стоп на 4, тейк на 10 - тейк исполняется только на
  // оставшиеся 6.
  const Timeline T = twoBars(B{0, 1, "100", "111", "94", "100"});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  B.Orders.push_back(stopOrder(0, Side::Sell, "95", 4));
  B.Orders.push_back(takeOrder(1, "110", 10));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 2u);
  EXPECT_TRUE(B.Fills[0].StopAndTakeSameBar);
  EXPECT_EQ(B.Fills[1].OrderId, 1);
  EXPECT_EQ(B.Fills[1].Lots, 6);
  EXPECT_FALSE(B.Fills[1].StopAndTakeSameBar);
  EXPECT_EQ(B.Orders[1].Status, OrderStatus::Cancelled);
  EXPECT_EQ(B.Orders[1].ReasonCode, Reason::InsufficientPosition);
}

TEST(StopBeforeTake, NoFlagWhenTakeNotReached) {
  const Timeline T = twoBars(B{0, 1, "100", "109", "94", "100"});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B = stopTakePair(true);
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_FALSE(B.Fills[0].StopAndTakeSameBar);
}

TEST(StopBeforeTake, TrailingStopCountsAsStop) {
  const Timeline T = twoBars(B{0, 1, "100", "111", "94", "100"});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  Order Trailing = makeOrder(0, Side::Sell, OrderType::TrailingStop, 10);
  Trailing.Spec.TrailingPercent = d("5");
  B.Orders.push_back(Trailing);
  B.Orders.push_back(takeOrder(1, "110"));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_TRUE(B.Fills[0].StopAndTakeSameBar);
}

// ---- Срок «день»
// -----------------------------------------------------------------------------------------

// День 1: шаги 0-2, день 2: шаги 3-5.
Timeline twoDays() {
  std::vector<B> List;
  for (int T = 0; T < 3; ++T)
    List.push_back(B{0, T, "101", "102", "100.5", "101"});
  for (int T = 0; T < 3; ++T)
    List.push_back(B{1, T, "101", "102", "100.5", "101"});
  return Timeline::build({kA}, {bars(kA, List)}, {});
}

TEST(Day, ExpiresEndOfFirstDay) {
  // Заявка, поданная на последнем шаге дня 1, действует весь день 2 и снимается
  // после его последнего шага.
  const Timeline T = twoDays();
  ASSERT_TRUE(T.isLastStepOfDay(2));
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  Order O = limitOrder(0, Side::Buy, "50.00");
  O.Spec.Tif = TimeInForce::Day;
  O.PlacedStep = 2;
  B.Orders.push_back(O);
  step(Engine, 2, T, B, Pf);
  EXPECT_EQ(B.Orders[0].Status,
            OrderStatus::Active); // последний шаг дня подачи - заявка ещё не
                                  // действует
  step(Engine, 3, T, B, Pf);
  step(Engine, 4, T, B, Pf);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Active);
  step(Engine, 5, T, B, Pf);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Expired);
  EXPECT_EQ(B.Orders[0].ReasonCode, Reason::DayExpired);
  EXPECT_EQ(B.Orders[0].ClosedStep, 5);
  ASSERT_EQ(B.Changed.size(), 1u);
  EXPECT_EQ(B.Changed[0], 0);
}

TEST(Day, PlacedMidDayExpiresEndOfSameDay) {
  const Timeline T = twoDays();
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  Order O = limitOrder(0, Side::Buy, "50.00");
  O.Spec.Tif = TimeInForce::Day;
  O.PlacedStep = 0;
  B.Orders.push_back(O);
  step(Engine, 1, T, B, Pf);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Active);
  step(Engine, 2, T, B, Pf);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Expired);
  EXPECT_EQ(B.Orders[0].ClosedStep, 2);
}

TEST(Day, PartiallyFilledOrderExpiresRemainderOnce) {
  // Исполнена часть на последнем шаге дня, остаток снимается там же; в changed
  // заявка одна.
  std::vector<B> List;
  List.push_back(B{0, 0, "101", "102", "100.5", "101", 50});
  List.push_back(B{0, 1, "99", "99.5", "98.5", "99", 50});
  const Timeline T = Timeline::build({kA}, {bars(kA, List)}, {});
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  Order O = limitOrder(0, Side::Buy, "100.00");
  O.Spec.Tif = TimeInForce::Day;
  B.Orders.push_back(O);
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].Lots, 5);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Expired);
  EXPECT_EQ(B.Orders[0].FilledLots, 5);
  EXPECT_EQ(B.Changed.size(), 1u);
}

TEST(Day, GtcIsNeverExpired) {
  const Timeline T = twoDays();
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  B.Orders.push_back(limitOrder(0, Side::Buy, "50.00"));
  for (int S = 1; S <= 5; ++S)
    step(Engine, S, T, B, Pf);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Active);
}

TEST(Day, OrderPlacedOnFinalStepIsLeftToSessionEnd) {
  const Timeline T = twoDays();
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  Order O = limitOrder(0, Side::Buy, "50.00");
  O.Spec.Tif = TimeInForce::Day;
  O.PlacedStep = 5;
  B.Orders.push_back(O);
  step(Engine, 5, T, B, Pf);
  EXPECT_EQ(B.Orders[0].Status, OrderStatus::Active);
}

// ---- Порядок и детерминизм
// --------------------------------------------------------------------------------

TEST(ConditionalOrders, ProcessedInIdOrderWithinGroup) {
  // V = 100 -> бюджет 10 лотов. Две лимитные покупки по 10 лотов: исполняется
  // меньший id, хотя в векторе он вторым.
  const Timeline T =
      Timeline::build({kA},
                      {bars(kA, {B{0, 0, "100", "100", "100", "100", 100},
                                 B{0, 1, "99", "99.5", "98.5", "99", 100}})},
                      {});
  const auto Engine = engineA();
  Portfolio Pf = holding(0);
  Book B;
  B.Orders.push_back(limitOrder(7, Side::Buy, "100.00"));
  B.Orders.push_back(limitOrder(3, Side::Buy, "100.00"));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].OrderId, 3);
}

TEST(ConditionalOrders, StopsRunBeforeLimits) {
  // V = 100 -> бюджет 10 лотов. Лимитная продажа (id 0) и стоп (id 1): стоп
  // идёт раньше и забирает бюджет.
  const Timeline T =
      Timeline::build({kA},
                      {bars(kA, {B{0, 0, "100", "100", "100", "100", 100},
                                 B{0, 1, "96", "101", "94", "95", 100}})},
                      {});
  const auto Engine = engineA();
  Portfolio Pf = holding(10);
  Book B;
  B.Orders.push_back(limitOrder(0, Side::Sell, "100.50"));
  B.Orders.push_back(stopOrder(1, Side::Sell, "95"));
  step(Engine, 1, T, B, Pf);
  ASSERT_EQ(B.Fills.size(), 1u);
  EXPECT_EQ(B.Fills[0].OrderId, 1);
  EXPECT_TRUE(B.Fills[0].StopAndTakeSameBar); // лимитная продажа 100.50 тоже
                                              // была бы задета (high 101)
}

} // namespace
