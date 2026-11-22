#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "support/fixture1.hpp"

namespace {

using core::CommandResult;
using core::CostConfig;
using core::Decimal;
using core::JournalEntry;
using core::Order;
using core::OrderSpec;
using core::OrderStatus;
using core::OrderType;
using core::PlaceCmd;
using core::PortfolioSnapshot;
using core::Reason;
using core::ReplaceCmd;
using core::SessionEngine;
using core::SessionSpec;
using core::Side;
using core::Timeframe;
using core::TimeInForce;
using core::Timeline;

Decimal d(const char *Text) { return Decimal::parse(Text); }

std::shared_ptr<const Timeline> fixtureTimeline() {
  auto F = core::testing::fixture1();
  return std::make_shared<const Timeline>(
      Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                     Timeframe::D1, 20240109, 20240122));
}

SessionSpec fixtureSpec() {
  SessionSpec Spec;
  Spec.Instruments = core::testing::fixture1().Instruments;
  Spec.Costs = CostConfig::defaults();
  Spec.InitialCash = d("1000000");
  return Spec;
}

OrderSpec market(Side OrderSide, std::int64_t Lots, int Instrument = 0) {
  OrderSpec S;
  S.Instrument = Instrument;
  S.OrderSide = OrderSide;
  S.Type = OrderType::Market;
  S.Lots = Lots;
  return S;
}

OrderSpec limit(Side OrderSide, std::int64_t Lots, const char *Price,
                int Instrument = 0) {
  OrderSpec S = market(OrderSide, Lots, Instrument);
  S.Type = OrderType::Limit;
  S.LimitPrice = d(Price);
  S.Tif = TimeInForce::Gtc;
  return S;
}

JournalEntry place(std::int64_t Seq, std::int64_t Step, OrderSpec Spec,
                   std::string RuleName = {}, std::string Because = {}) {
  JournalEntry E;
  E.Seq = Seq;
  E.Step = Step;
  E.Command = PlaceCmd{std::move(Spec), std::nullopt};
  E.RuleName = std::move(RuleName);
  E.Because = std::move(Because);
  return E;
}

JournalEntry replace(std::int64_t Seq, std::int64_t Step, std::int64_t OrderId,
                     OrderSpec Spec, std::string RuleName = {},
                     std::string Because = {}) {
  JournalEntry E;
  E.Seq = Seq;
  E.Step = Step;
  E.Command = ReplaceCmd{OrderId, PlaceCmd{std::move(Spec), std::nullopt}};
  E.RuleName = std::move(RuleName);
  E.Because = std::move(Because);
  return E;
}

const Order &orderById(const SessionEngine &Engine, std::int64_t Id) {
  for (const Order &O : Engine.orders()) {
    if (O.Id == Id)
      return O;
  }
  throw std::out_of_range("нет заявки");
}

TEST(BarsInPositionTest, LifecycleScaleInPartialSellSkippedBarAndClose) {
  SessionEngine Engine(fixtureSpec(), fixtureTimeline());

  const PortfolioSnapshot S0 = Engine.snapshot();
  const std::vector<std::optional<std::int64_t>> ExpectedEmpty{std::nullopt,
                                                               std::nullopt};
  EXPECT_EQ(S0.BarsInPosition, ExpectedEmpty);
  EXPECT_EQ(S0.PositionReopenedOnBar, (std::vector<bool>{false, false}));

  ASSERT_TRUE(Engine.apply(place(0, 0, market(Side::Buy, 5, 0))).Accepted);
  ASSERT_TRUE(Engine.apply(place(1, 0, market(Side::Buy, 5, 1))).Accepted);
  Engine.advance(); // шаг 1: открытие обеих позиций

  const PortfolioSnapshot S1 = Engine.snapshot();
  EXPECT_EQ(S1.BarsInPosition,
            (std::vector<std::optional<std::int64_t>>{0, 0}));
  EXPECT_EQ(S1.PositionReopenedOnBar, (std::vector<bool>{false, false}));

  // Докупка по A: счётчик не сбрасывается и увеличивается до 1.
  ASSERT_TRUE(Engine.apply(place(2, 1, market(Side::Buy, 3, 0))).Accepted);
  Engine.advance(); // шаг 2

  const PortfolioSnapshot S2 = Engine.snapshot();
  EXPECT_EQ(S2.BarsInPosition,
            (std::vector<std::optional<std::int64_t>>{1, 1}));
  EXPECT_EQ(S2.PositionReopenedOnBar, (std::vector<bool>{false, false}));

  // Частичная продажа по A (8 -> 4) и пропуск бара у B на шаге 3.
  ASSERT_TRUE(Engine.apply(place(3, 2, market(Side::Sell, 4, 0))).Accepted);
  Engine.advance(); // шаг 3: у FIXB бара нет

  const PortfolioSnapshot S3 = Engine.snapshot();
  EXPECT_EQ(S3.BarsInPosition,
            (std::vector<std::optional<std::int64_t>>{2, 1}));
  EXPECT_EQ(S3.PositionReopenedOnBar, (std::vector<bool>{false, false}));

  Engine.advance(); // шаг 4: у FIXB снова есть бар

  const PortfolioSnapshot S4 = Engine.snapshot();
  EXPECT_EQ(S4.BarsInPosition,
            (std::vector<std::optional<std::int64_t>>{3, 2}));
  EXPECT_EQ(S4.PositionReopenedOnBar, (std::vector<bool>{false, false}));

  // Полное закрытие позиции по A (4 -> 0).
  ASSERT_TRUE(Engine.apply(place(4, 4, market(Side::Sell, 4, 0))).Accepted);
  Engine.advance(); // шаг 5

  const PortfolioSnapshot S5 = Engine.snapshot();
  EXPECT_EQ(S5.BarsInPosition,
            (std::vector<std::optional<std::int64_t>>{std::nullopt, 3}));
  EXPECT_EQ(S5.PositionReopenedOnBar, (std::vector<bool>{false, false}));
}

TEST(BarsInPositionTest, CloseAndReopenSameBarAndEngineCopy) {
  SessionEngine Engine(fixtureSpec(), fixtureTimeline());
  ASSERT_TRUE(Engine.apply(place(0, 0, market(Side::Buy, 5, 0))).Accepted);
  Engine.advance(); // шаг 1: BarsInPosition = 0
  Engine.advance(); // шаг 2: BarsInPosition = 1

  // На шаге 3 рыночная продажа 5 лотов (идёт первой) закрывает позицию до 0,
  // а лимитная покупка 3 лотов по 106 открывает её заново на том же баре.
  ASSERT_TRUE(Engine.apply(place(1, 2, market(Side::Sell, 5, 0))).Accepted);
  ASSERT_TRUE(
      Engine.apply(place(2, 2, limit(Side::Buy, 3, "106", 0))).Accepted);
  Engine.advance(); // шаг 3

  EXPECT_EQ(Engine.portfolio().lots(0), 3);
  const PortfolioSnapshot S3 = Engine.snapshot();
  EXPECT_EQ(S3.BarsInPosition[0], 0);
  EXPECT_TRUE(S3.PositionReopenedOnBar[0]);
  EXPECT_EQ(Engine.lastStepEvents().Portfolio.BarsInPosition[0], 0);
  EXPECT_TRUE(Engine.lastStepEvents().Portfolio.PositionReopenedOnBar[0]);

  // Копирование движка (INV-8) сохраняет счётчик и флаг переоткрытия.
  SessionEngine Copy = Engine;
  EXPECT_EQ(Copy.snapshot().BarsInPosition, S3.BarsInPosition);
  EXPECT_EQ(Copy.snapshot().PositionReopenedOnBar, S3.PositionReopenedOnBar);

  Engine.advance(); // шаг 4: держим позицию
  EXPECT_EQ(Engine.snapshot().BarsInPosition[0], 1);
  EXPECT_FALSE(Engine.snapshot().PositionReopenedOnBar[0]);

  // В копии закрываем позицию: состояния расходятся независимо.
  ASSERT_TRUE(Copy.apply(place(3, 3, market(Side::Sell, 3, 0))).Accepted);
  Copy.advance(); // шаг 4
  EXPECT_EQ(Copy.snapshot().BarsInPosition[0], std::nullopt);
  EXPECT_FALSE(Copy.snapshot().PositionReopenedOnBar[0]);
}

TEST(BarsInPositionTest, RuleNameAndBecausePropagatedOnPlaceAndReplace) {
  SessionEngine Engine(fixtureSpec(), fixtureTimeline());

  const CommandResult Placed = Engine.apply(
      place(0, 0, limit(Side::Buy, 2, "95", 0), "entry_rule", "rsi < 30"));
  ASSERT_TRUE(Placed.Accepted);
  ASSERT_EQ(Placed.OrderIds.size(), 1u);
  const std::int64_t OriginalId = Placed.OrderIds[0];
  EXPECT_EQ(orderById(Engine, OriginalId).RuleName, "entry_rule");
  EXPECT_EQ(orderById(Engine, OriginalId).Because, "rsi < 30");

  // Отклонённая PlaceCmd тоже сохраняет RuleName и Because в отклонённой
  // заявке.
  const CommandResult RejectedPlace = Engine.apply(
      place(1, 0, market(Side::Buy, 1'000'000, 0), "bad_rule", "all in"));
  ASSERT_FALSE(RejectedPlace.Accepted);
  ASSERT_EQ(RejectedPlace.OrderIds.size(), 1u);
  const Order &RejectedOrder = orderById(Engine, RejectedPlace.OrderIds[0]);
  EXPECT_EQ(RejectedOrder.Status, OrderStatus::Rejected);
  EXPECT_EQ(RejectedOrder.RuleName, "bad_rule");
  EXPECT_EQ(RejectedOrder.Because, "all in");

  // Отклонённая ReplaceCmd оставляет прежнюю активную заявку с её Because.
  const OrderSpec BadLimit = limit(Side::Buy, 1'000'000, "96", 0);
  const CommandResult RejectedReplace = Engine.apply(
      replace(2, 0, OriginalId, BadLimit, "replace_fail", "too big"));
  ASSERT_FALSE(RejectedReplace.Accepted);
  ASSERT_EQ(RejectedReplace.OrderIds.size(), 1u);
  EXPECT_EQ(orderById(Engine, OriginalId).Status, OrderStatus::Active);
  EXPECT_EQ(orderById(Engine, OriginalId).RuleName, "entry_rule");
  EXPECT_EQ(orderById(Engine, OriginalId).Because, "rsi < 30");
  const Order &RejectedReplOrder =
      orderById(Engine, RejectedReplace.OrderIds[0]);
  EXPECT_EQ(RejectedReplOrder.Status, OrderStatus::Rejected);
  EXPECT_EQ(RejectedReplOrder.RuleName, "replace_fail");
  EXPECT_EQ(RejectedReplOrder.Because, "too big");

  // Успешная ReplaceCmd закрывает старую (с её прежним Because) и создаёт
  // новую с новым RuleName/Because.
  const CommandResult AcceptedReplace = Engine.apply(replace(
      3, 0, OriginalId, limit(Side::Buy, 3, "96", 0), "replace_ok", "raise"));
  ASSERT_TRUE(AcceptedReplace.Accepted);
  ASSERT_EQ(AcceptedReplace.OrderIds.size(), 1u);
  EXPECT_EQ(orderById(Engine, OriginalId).Status, OrderStatus::Cancelled);
  EXPECT_EQ(orderById(Engine, OriginalId).ReasonCode, Reason::Replaced);
  EXPECT_EQ(orderById(Engine, OriginalId).RuleName, "entry_rule");
  EXPECT_EQ(orderById(Engine, OriginalId).Because, "rsi < 30");

  const Order &ReplacedOrder = orderById(Engine, AcceptedReplace.OrderIds[0]);
  EXPECT_EQ(ReplacedOrder.Status, OrderStatus::Active);
  EXPECT_EQ(ReplacedOrder.RuleName, "replace_ok");
  EXPECT_EQ(ReplacedOrder.Because, "raise");
}

} // namespace
