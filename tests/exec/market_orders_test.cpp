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

Decimal d(const char *Text) { return Decimal::parse(Text); }

constexpr std::int64_t kMinuteMs = 60'000;
constexpr std::int64_t kStart =
    1'704'747'600'000 + 10 * 3'600'000; // 2024-01-09 10:00 МСК

const Instrument kA{"A", "FIXA", 10, Decimal::parse("0.01")};
const Instrument kB{"B", "FIXB", 10000, Decimal::parse("0.000005")};

// Бар минутной шкалы: номер минуты t, цены и объём в лотах (lotSize нужен для
// пересчёта в штуки).
struct B {
  int T;
  const char *Open;
  const char *Close;
  std::int64_t VolumeLots;
};

std::vector<Bar> bars(const Instrument &Inst, const std::vector<B> &List) {
  std::vector<Bar> Out;
  for (const B &B : List) {
    Bar Bar;
    Bar.TsOpenMs = kStart + B.T * kMinuteMs;
    Bar.AvailableAtMs = Bar.TsOpenMs + kMinuteMs;
    Bar.Open = d(B.Open);
    Bar.Close = d(B.Close);
    Bar.High = Bar.Low = Bar.Open;
    if (Bar.Close > Bar.High)
      Bar.High = Bar.Close;
    if (Bar.Close < Bar.Low)
      Bar.Low = Bar.Close;
    Bar.Volume = B.VolumeLots * Inst.LotSize;
    Out.push_back(Bar);
  }
  return Out;
}

// Бумага A - бары на шагах 0…5 (open 100 + шаг, close open + 0.5); V - объём
// каждого бара.
std::vector<B> seriesA(std::int64_t VolumeLots) {
  static const char *Opens[] = {"100", "101", "102", "103", "104", "105"};
  static const char *Closes[] = {"100.5", "101.5", "102.5",
                                 "103.5", "104.5", "105.5"};
  std::vector<B> Out;
  for (int I = 0; I < 6; ++I)
    Out.push_back(B{I, Opens[I], Closes[I], VolumeLots});
  return Out;
}

Order market(std::int64_t Id, int Instrument, Side Side, std::int64_t Lots,
             std::int64_t PlacedStep) {
  Order O;
  O.Id = Id;
  O.Spec.Instrument = Instrument;
  O.Spec.OrderSide = Side;
  O.Spec.Type = OrderType::Market;
  O.Spec.Lots = Lots;
  O.PlacedStep = PlacedStep;
  return O;
}

struct Book {
  std::vector<Order> Orders;
  std::vector<Fill> Fills;
  std::vector<std::int64_t> Changed;
};

TEST(MarketOrders, NotBeforeNextBar) {
  // INV-2: заявка шага 0 исполняется на шаге 1 по open шага 1 (101), а не по
  // close шага 0.
  const Timeline T = Timeline::build({kA}, {bars(kA, seriesA(50000))}, {});
  const ExecutionEngine Engine({kA}, CostConfig::defaults());
  Portfolio Pf(d("1000000"), 1);
  Book R;
  R.Orders.push_back(market(0, 0, Side::Buy, 100, 0));

  Engine.processStep(0, T, R.Orders, Pf, R.Fills, R.Changed);
  EXPECT_TRUE(R.Fills.empty());
  EXPECT_TRUE(R.Changed.empty());
  EXPECT_EQ(R.Orders[0].Status, OrderStatus::Active);
  EXPECT_EQ(Pf.lots(0), 0);

  Engine.processStep(1, T, R.Orders, Pf, R.Fills, R.Changed);
  ASSERT_EQ(R.Fills.size(), 1u);
  const Fill &F = R.Fills[0];
  EXPECT_EQ(F.OrderId, 0);
  EXPECT_EQ(F.Step, 1);
  EXPECT_EQ(F.BasePrice, d("101"));
  EXPECT_EQ(F.Price, d("101.08")); // 101 * 1.0007 = 101.0707 -> шаг 0.01 вверх
  EXPECT_EQ(F.Lots, 100);
  EXPECT_EQ(R.Orders[0].Status, OrderStatus::Filled);
  EXPECT_EQ(R.Orders[0].ReasonCode, Reason::None);
  EXPECT_EQ(R.Orders[0].FilledLots, 100);
  EXPECT_EQ(R.Orders[0].ClosedStep, 1);
  ASSERT_EQ(R.Changed.size(), 1u);
  EXPECT_EQ(R.Changed[0], 0);
  EXPECT_EQ(Pf.lots(0), 100);
}

TEST(MarketOrders, FillRecordsAmountsAndCostSplit) {
  std::vector<B> A = seriesA(50000);
  A[1].Open = "271.35";
  const Timeline T = Timeline::build({kA}, {bars(kA, A)}, {});
  const ExecutionEngine Engine({kA}, CostConfig::defaults());
  Portfolio Pf(d("1000000"), 1);
  Book R;
  R.Orders.push_back(market(0, 0, Side::Buy, 100, 0));
  Engine.processStep(1, T, R.Orders, Pf, R.Fills, R.Changed);
  ASSERT_EQ(R.Fills.size(), 1u);
  const Fill &F = R.Fills[0];
  EXPECT_EQ(F.Price, d("271.54"));
  EXPECT_EQ(F.Value, d("271540.00"));
  EXPECT_EQ(F.Commission, d("814.62"));
  EXPECT_EQ(F.Fee, d("27.16"));
  EXPECT_EQ(F.SpreadCost, d("135.68"));
  EXPECT_EQ(F.SlippageCost, d("54.32"));
  EXPECT_FALSE(F.StopAndTakeSameBar);
  EXPECT_EQ(Pf.cash(),
            d("1000000") - d("271540.00") - d("814.62") - d("27.16"));
}

TEST(MarketOrders, WaitsForOwnInstrumentBar) {
  // У B нет бара на шаге 3: заявка B шага 2 исполняется на шаге 4, по open бара
  // B, не по бару A.
  std::vector<B> B = {{0, "0.0240", "0.0240", 90000},
                      {1, "0.0242", "0.0242", 90000},
                      {2, "0.0244", "0.0244", 90000},
                      {4, "0.0245", "0.0245", 90000},
                      {5, "0.0246", "0.0246", 90000}};
  const Timeline T =
      Timeline::build({kA, kB}, {bars(kA, seriesA(50000)), bars(kB, B)}, {});
  ASSERT_EQ(T.barOf(1, 3), nullptr);
  const ExecutionEngine Engine({kA, kB}, CostConfig::defaults());
  Portfolio Pf(d("1000000"), 2);
  Book R;
  R.Orders.push_back(market(0, 1, Side::Buy, 300, 2));

  Engine.processStep(3, T, R.Orders, Pf, R.Fills, R.Changed);
  EXPECT_TRUE(R.Fills.empty());
  EXPECT_EQ(R.Orders[0].Status, OrderStatus::Active);

  Engine.processStep(4, T, R.Orders, Pf, R.Fills, R.Changed);
  ASSERT_EQ(R.Fills.size(), 1u);
  EXPECT_EQ(R.Fills[0].Step, 4);
  EXPECT_EQ(R.Fills[0].Instrument, 1);
  EXPECT_EQ(R.Fills[0].BasePrice, d("0.0245"));
  EXPECT_EQ(R.Fills[0].Price, d("0.024525"));
  EXPECT_EQ(R.Orders[0].Status, OrderStatus::Filled);
  EXPECT_EQ(Pf.lots(1), 300);
  EXPECT_EQ(Pf.lots(0), 0);
}

TEST(MarketOrders, ZeroLiquidityBudget) {
  // V = 9 лотов: floor(0.1 * 9) = 0 - заявка снимается, исполнений нет,
  // портфель не тронут.
  const Timeline T = Timeline::build({kA}, {bars(kA, seriesA(9))}, {});
  const ExecutionEngine Engine({kA}, CostConfig::defaults());
  Portfolio Pf(d("1000000"), 1);
  Book R;
  R.Orders.push_back(market(0, 0, Side::Buy, 5, 0));
  Engine.processStep(1, T, R.Orders, Pf, R.Fills, R.Changed);
  EXPECT_TRUE(R.Fills.empty());
  EXPECT_EQ(R.Orders[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(R.Orders[0].ReasonCode, Reason::LiquidityLimit);
  EXPECT_EQ(R.Orders[0].FilledLots, 0);
  EXPECT_EQ(R.Orders[0].ClosedStep, 1);
  ASSERT_EQ(R.Changed.size(), 1u);
  EXPECT_EQ(Pf.cash(), d("1000000"));
}

TEST(MarketOrders, PartialByLiquidity) {
  const Timeline T = Timeline::build({kA}, {bars(kA, seriesA(2000))}, {});
  const ExecutionEngine Engine({kA}, CostConfig::defaults());
  Portfolio Pf(d("1000000"), 1);
  Book R;
  R.Orders.push_back(market(0, 0, Side::Buy, 300, 0));
  Engine.processStep(1, T, R.Orders, Pf, R.Fills, R.Changed);
  ASSERT_EQ(R.Fills.size(), 1u);
  EXPECT_EQ(R.Fills[0].Lots, 200);
  EXPECT_EQ(R.Orders[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(R.Orders[0].FilledLots, 200);
  EXPECT_EQ(R.Orders[0].ReasonCode, Reason::LiquidityLimit);
  EXPECT_EQ(Pf.lots(0), 200);
}

TEST(MarketOrders, LiquidityBudgetIsSharedByOrdersOfStep) {
  // Бюджет 200 лотов на два заявки: 150 + 50, вторая снимается с
  // LIQUIDITY_LIMIT; порядок - по id.
  const Timeline T = Timeline::build({kA}, {bars(kA, seriesA(2000))}, {});
  const ExecutionEngine Engine({kA}, CostConfig::defaults());
  Portfolio Pf(d("1000000"), 1);
  Book R;
  R.Orders.push_back(
      market(5, 0, Side::Buy, 100, 0)); // в векторе первая, но id больше
  R.Orders.push_back(market(2, 0, Side::Buy, 150, 0));
  Engine.processStep(1, T, R.Orders, Pf, R.Fills, R.Changed);
  ASSERT_EQ(R.Fills.size(), 2u);
  EXPECT_EQ(R.Fills[0].OrderId, 2);
  EXPECT_EQ(R.Fills[0].Lots, 150);
  EXPECT_EQ(R.Fills[1].OrderId, 5);
  EXPECT_EQ(R.Fills[1].Lots, 50);
  EXPECT_EQ(R.Orders[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(R.Orders[0].ReasonCode, Reason::LiquidityLimit);
  EXPECT_EQ(R.Orders[1].Status, OrderStatus::Filled);
  EXPECT_EQ(R.Changed, (std::vector<std::int64_t>{2, 5}));
}

TEST(MarketOrders, ReducedByCash) {
  // Денег на 3 лота с издержками (≈ 3033.6 ₽), четвёртый уже не проходит.
  const Timeline T = Timeline::build({kA}, {bars(kA, seriesA(50000))}, {});
  const CostConfig Costs = CostConfig::defaults();
  const ExecutionEngine Engine({kA}, Costs);
  Portfolio Pf(d("3500"), 1);
  Book R;
  R.Orders.push_back(market(0, 0, Side::Buy, 10, 0));
  Engine.processStep(1, T, R.Orders, Pf, R.Fills, R.Changed);
  ASSERT_EQ(R.Fills.size(), 1u);
  EXPECT_EQ(R.Fills[0].Lots, 3);
  EXPECT_EQ(R.Orders[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(R.Orders[0].FilledLots, 3);
  EXPECT_EQ(R.Orders[0].ReasonCode, Reason::InsufficientCash);
  EXPECT_GE(Pf.cash(), d("0"));
  // Четвёртого лота не хватило бы (цена для 4 лотов пересчитывается).
  const Decimal P4 =
      core::fillPrice(Side::Buy, d("101"), 4, 50000, Costs, kA.PriceStep);
  const core::Amounts A4 = core::amounts(P4, 4, kA.LotSize, Costs);
  EXPECT_GT(A4.Value + A4.Commission + A4.Fee, d("3500"));
}

TEST(MarketOrders, NoCashForSingleLot) {
  const Timeline T = Timeline::build({kA}, {bars(kA, seriesA(50000))}, {});
  const ExecutionEngine Engine({kA}, CostConfig::defaults());
  Portfolio Pf(d("500"), 1);
  Book R;
  R.Orders.push_back(market(0, 0, Side::Buy, 10, 0));
  Engine.processStep(1, T, R.Orders, Pf, R.Fills, R.Changed);
  EXPECT_TRUE(R.Fills.empty());
  EXPECT_EQ(R.Orders[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(R.Orders[0].ReasonCode, Reason::InsufficientCash);
  EXPECT_EQ(Pf.cash(), d("500"));
}

TEST(MarketOrders, SellReducedByPosition) {
  const Timeline T = Timeline::build({kA}, {bars(kA, seriesA(50000))}, {});
  const ExecutionEngine Engine({kA}, CostConfig::defaults());
  Portfolio Pf(d("1000000"), 1);
  Pf.applyFill(0, Side::Buy, 5, d("100"), d("5000.00"), d("2.50"), d("0.50"), 0,
               99);
  Book R;
  R.Orders.push_back(market(0, 0, Side::Sell, 8, 0));
  Engine.processStep(1, T, R.Orders, Pf, R.Fills, R.Changed);
  ASSERT_EQ(R.Fills.size(), 1u);
  EXPECT_EQ(R.Fills[0].Lots, 5);
  EXPECT_EQ(R.Fills[0].OrderSide, Side::Sell);
  EXPECT_LE(R.Fills[0].Price, d("101")); // продажа - не выше open
  EXPECT_EQ(R.Orders[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(R.Orders[0].ReasonCode, Reason::InsufficientPosition);
  EXPECT_EQ(Pf.lots(0), 0);
}

TEST(MarketOrders, SellWithoutPositionIsCancelled) {
  const Timeline T = Timeline::build({kA}, {bars(kA, seriesA(50000))}, {});
  const ExecutionEngine Engine({kA}, CostConfig::defaults());
  Portfolio Pf(d("1000000"), 1);
  Book R;
  R.Orders.push_back(market(0, 0, Side::Sell, 1, 0));
  Engine.processStep(1, T, R.Orders, Pf, R.Fills, R.Changed);
  EXPECT_TRUE(R.Fills.empty());
  EXPECT_EQ(R.Orders[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(R.Orders[0].ReasonCode, Reason::InsufficientPosition);
}

TEST(MarketOrders, ClosedAndUnreachedConditionalOrdersAreUntouched) {
  const Timeline T = Timeline::build({kA}, {bars(kA, seriesA(50000))}, {});
  const ExecutionEngine Engine({kA}, CostConfig::defaults());
  Portfolio Pf(d("1000000"), 1);
  Book R;
  R.Orders.push_back(market(0, 0, Side::Buy, 1, 0));
  R.Orders[0].Spec.Type = OrderType::Limit;
  R.Orders[0].Spec.LimitPrice = d("50"); // бар шага 1 (low 101) до уровня не
                                         // доходит - лимитная остаётся активной
  R.Orders.push_back(market(1, 0, Side::Buy, 1, 0));
  R.Orders[1].Status = OrderStatus::Cancelled;
  Engine.processStep(1, T, R.Orders, Pf, R.Fills, R.Changed);
  EXPECT_TRUE(R.Fills.empty());
  EXPECT_TRUE(R.Changed.empty());
  EXPECT_EQ(R.Orders[0].Status, OrderStatus::Active);
  EXPECT_EQ(R.Orders[1].Status, OrderStatus::Cancelled);
}

TEST(MarketOrders, NoLiquidityLimitWhenShareUnset) {
  // Без лимита ликвидности заявка идёт целиком, проскальзывание всё
  // равно считается от объёма.
  CostConfig Costs = CostConfig::defaults();
  Costs.LiquidityShare.reset();
  const Timeline T = Timeline::build({kA}, {bars(kA, seriesA(2000))}, {});
  const ExecutionEngine Engine({kA}, Costs);
  Portfolio Pf(d("1000000"), 1);
  Book R;
  R.Orders.push_back(market(0, 0, Side::Buy, 300, 0));
  Engine.processStep(1, T, R.Orders, Pf, R.Fills, R.Changed);
  ASSERT_EQ(R.Fills.size(), 1u);
  EXPECT_EQ(R.Fills[0].Lots, 300);
  EXPECT_EQ(R.Orders[0].Status, OrderStatus::Filled);
}

TEST(MarketOrders, ZeroVolumeBarWithoutLiquidityLimitStillFills) {
  // Без лимита ликвидности нулевой объём не снимает заявку:
  // проскальзывания нет, остаётся спред.
  CostConfig Costs = CostConfig::defaults();
  Costs.LiquidityShare.reset();
  const Timeline T = Timeline::build({kA}, {bars(kA, seriesA(0))}, {});
  const ExecutionEngine Engine({kA}, Costs);
  Portfolio Pf(d("1000000"), 1);
  Book R;
  R.Orders.push_back(market(0, 0, Side::Buy, 100, 0));
  Engine.processStep(1, T, R.Orders, Pf, R.Fills, R.Changed);
  ASSERT_EQ(R.Fills.size(), 1u);
  EXPECT_EQ(R.Fills[0].Lots, 100);
  EXPECT_EQ(R.Fills[0].Price,
            d("101.06")); // 101 * 1.0005 = 101.0505 -> шаг вверх
  EXPECT_EQ(R.Orders[0].Status, OrderStatus::Filled);
}

TEST(MarketOrders, ZeroVolumeBarWithLiquidityLimitIsCancelled) {
  const Timeline T = Timeline::build({kA}, {bars(kA, seriesA(0))}, {});
  const ExecutionEngine Engine({kA}, CostConfig::defaults());
  Portfolio Pf(d("1000000"), 1);
  Book R;
  R.Orders.push_back(market(0, 0, Side::Buy, 1, 0));
  Engine.processStep(1, T, R.Orders, Pf, R.Fills, R.Changed);
  EXPECT_TRUE(R.Fills.empty());
  EXPECT_EQ(R.Orders[0].ReasonCode, Reason::LiquidityLimit);
}

TEST(MarketOrders, SellFarAboveBarVolumeWithoutLimitDoesNotCrash) {
  // Продажа 100 лотов при V = 10 лотов без лимита ликвидности: цена зажата
  // снизу шагом цены, прогон не падает.
  CostConfig Costs = CostConfig::defaults();
  Costs.LiquidityShare.reset();
  const Timeline T = Timeline::build({kA}, {bars(kA, seriesA(10))}, {});
  const ExecutionEngine Engine({kA}, Costs);
  Portfolio Pf(d("1000000"), 1);
  Pf.applyFill(0, Side::Buy, 100, d("100"), d("100000.00"), d("50.00"),
               d("10.00"), 0, 99);
  Book R;
  R.Orders.push_back(market(0, 0, Side::Sell, 100, 0));
  Engine.processStep(1, T, R.Orders, Pf, R.Fills, R.Changed);
  ASSERT_EQ(R.Fills.size(), 1u);
  EXPECT_EQ(R.Fills[0].Price, d("0.01"));
  EXPECT_EQ(R.Orders[0].Status, OrderStatus::Filled);
  EXPECT_EQ(Pf.lots(0), 0);
}

TEST(LiquidityBudget, IsSharedAndFloored) {
  const Timeline T = Timeline::build({kA}, {bars(kA, seriesA(2005))}, {});
  const core::StepBar &Bar = *T.barOf(0, 1);
  const CostConfig Costs = CostConfig::defaults();
  core::LiquidityBudget Budget(1);
  EXPECT_EQ(Budget.remaining(0, Bar, Costs), 200); // floor(0.1 * 2005)
  Budget.consume(0, 150);
  EXPECT_EQ(Budget.remaining(0, Bar, Costs),
            50); // второй вызов видит остаток, а не пересчитывает
  Budget.consume(0, 50);
  EXPECT_EQ(Budget.remaining(0, Bar, Costs), 0);
}

TEST(LiquidityBudget, UnlimitedWithoutShare) {
  const Timeline T = Timeline::build({kA}, {bars(kA, seriesA(0))}, {});
  CostConfig Costs = CostConfig::defaults();
  Costs.LiquidityShare.reset();
  core::LiquidityBudget Budget(1);
  EXPECT_FALSE(Budget.remaining(0, *T.barOf(0, 1), Costs).has_value());
  Budget.consume(0, 5);
  EXPECT_FALSE(Budget.remaining(0, *T.barOf(0, 1), Costs).has_value());
}

} // namespace
