#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "core/engine/digest.hpp"
#include "core/engine/session_engine.hpp"
#include "core/market/date_utils.hpp"
#include "core/market/timeline.hpp"

namespace {

using core::Bar;
using core::CostConfig;
using core::Decimal;
using core::ExecutionRules;
using core::Instrument;
using core::JournalEntry;
using core::OrderSpec;
using core::OrderStatus;
using core::OrderType;
using core::PlaceCmd;
using core::Reason;
using core::ReplayResult;
using core::SessionSpec;
using core::Side;
using core::TimeInForce;
using core::Timeline;
using core::TradingDay;

Decimal d(const char *Text) { return Decimal::parse(Text); }

Instrument makeInst(const char *Key = "A", std::int64_t LotSize = 10) {
  return Instrument{Key, Key, LotSize, d("0.01")};
}

Bar makeBar(std::int32_t Ymd, std::int64_t Hour, const char *Open,
            const char *Close, std::int64_t Volume = 100) {
  Bar B;
  B.TsOpenMs = core::moscowDayStartMs(Ymd) + Hour * core::kHourMs;
  B.AvailableAtMs = B.TsOpenMs + core::kHourMs;
  B.Open = d(Open);
  B.High = std::max(B.Open, d(Close));
  B.Low = std::min(B.Open, d(Close));
  B.Close = d(Close);
  B.Volume = Volume;
  return B;
}

std::shared_ptr<const Timeline>
makeTimeline(const std::vector<Bar> &Bars,
             const std::vector<std::int32_t> &Dates) {
  std::vector<TradingDay> Cal;
  Cal.reserve(Dates.size());
  for (std::int32_t Ymd : Dates)
    Cal.push_back(TradingDay{Ymd, true});
  return std::make_shared<const Timeline>(
      Timeline::build({makeInst("A", 10)}, {Bars}, Cal));
}

OrderSpec limitOrder(Side OrderSide, std::int64_t Lots, const char *Price,
                     TimeInForce Tif = TimeInForce::Gtc) {
  OrderSpec S;
  S.Instrument = 0;
  S.OrderSide = OrderSide;
  S.Type = OrderType::Limit;
  S.Lots = Lots;
  S.LimitPrice = d(Price);
  S.Tif = Tif;
  return S;
}

OrderSpec marketOrder(Side OrderSide, std::int64_t Lots,
                      TimeInForce Tif = TimeInForce::Gtc) {
  OrderSpec S;
  S.Instrument = 0;
  S.OrderSide = OrderSide;
  S.Type = OrderType::Market;
  S.Lots = Lots;
  S.Tif = Tif;
  return S;
}

JournalEntry placeAt(std::int64_t Seq, std::int64_t Step, OrderSpec Spec) {
  JournalEntry E;
  E.Seq = Seq;
  E.Step = Step;
  E.Command = PlaceCmd{std::move(Spec), std::nullopt};
  return E;
}

TEST(AdvancedOrders, RulesAndDisplayLotsValidation) {
  ExecutionRules BadNeg;
  BadNeg.PriceBandPct = d("-5");
  EXPECT_THROW(core::validateExecutionRules(BadNeg), std::invalid_argument);

  ExecutionRules BadHundred;
  BadHundred.PriceBandPct = d("100");
  EXPECT_THROW(core::validateExecutionRules(BadHundred), std::invalid_argument);

  const std::vector<Bar> Bars = {
      makeBar(20240110, 10, "100", "100"),
      makeBar(20240111, 10, "100", "100"),
  };
  const auto Tl = makeTimeline(Bars, {20240110, 20240111});
  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs = CostConfig::zero();
  Spec.InitialCash = d("100000");

  OrderSpec BadIce = limitOrder(Side::Buy, 5, "100");
  BadIce.DisplayLots = 6;
  const std::vector<JournalEntry> Journal = {placeAt(0, 0, BadIce)};
  const ReplayResult Res = core::replaySession(Spec, Tl, Journal, 1, 0);
  ASSERT_EQ(Res.CommandResults.size(), 1u);
  EXPECT_FALSE(Res.CommandResults[0].Accepted);
  EXPECT_EQ(Res.CommandResults[0].ReasonCode, Reason::InvalidParams);
  EXPECT_EQ(Res.CommandResults[0].Field, "displayLots");
}

TEST(AdvancedOrders, IocPartialFillAndNoRest) {
  const std::vector<Bar> Bars = {
      makeBar(20240110, 10, "100", "100", 40),
      makeBar(20240111, 10, "100", "100", 40),
      makeBar(20240112, 10, "100", "100", 40),
  };
  const auto Tl = makeTimeline(Bars, {20240110, 20240111, 20240112});

  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs = CostConfig::zero();
  Spec.Costs.LiquidityShare = d("0.10"); // 4 лота на бар
  Spec.InitialCash = d("100000");

  // 1) IOC на 10 лотов по 100: исполняется 4 лота на шаге 1, остаток снят.
  // 2) IOC на 5 лотов по 90 (не задет): 0 лотов, сразу снят на шаге 1.
  const std::vector<JournalEntry> Journal = {
      placeAt(0, 0, limitOrder(Side::Buy, 10, "100", TimeInForce::Ioc)),
      placeAt(1, 0, limitOrder(Side::Buy, 5, "90", TimeInForce::Ioc)),
  };
  const ReplayResult Res = core::replaySession(Spec, Tl, Journal, 2, 0);
  ASSERT_EQ(Res.Engine.orders().size(), 2u);
  EXPECT_EQ(Res.Engine.orders()[0].FilledLots, 4);
  EXPECT_EQ(Res.Engine.orders()[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(Res.Engine.orders()[0].ReasonCode, Reason::ImmediateOrCancel);
  EXPECT_EQ(Res.Engine.orders()[0].ClosedStep, 1);

  EXPECT_EQ(Res.Engine.orders()[1].FilledLots, 0);
  EXPECT_EQ(Res.Engine.orders()[1].Status, OrderStatus::Cancelled);
  EXPECT_EQ(Res.Engine.orders()[1].ReasonCode, Reason::ImmediateOrCancel);
  EXPECT_EQ(Res.Engine.orders()[1].ClosedStep, 1);
  // На шаге 2 новых исполнений нет (IOC не переносится).
  ASSERT_EQ(Res.Engine.fills().size(), 1u);
}

TEST(AdvancedOrders, FokAllOrNothing) {
  const std::vector<Bar> Bars = {
      makeBar(20240110, 10, "100", "100", 40),
      makeBar(20240111, 10, "100", "100", 40),
  };
  const auto Tl = makeTimeline(Bars, {20240110, 20240111});

  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs = CostConfig::zero();
  Spec.Costs.LiquidityShare = d("0.10"); // 4 лота на бар
  Spec.InitialCash = d("100000");

  // 1) Рыночная FOK на 10 лотов (> 4) -> 0 лотов, бюджет не потрачен!
  // 2) Лимитная FOK на 10 лотов (> 4) -> 0 лотов, бюджет не потрачен!
  // 3) Лимитная FOK на 4 лота (== 4) -> исполнена целиком!
  const std::vector<JournalEntry> Journal = {
      placeAt(0, 0, marketOrder(Side::Buy, 10, TimeInForce::Fok)),
      placeAt(1, 0, limitOrder(Side::Buy, 10, "100", TimeInForce::Fok)),
      placeAt(2, 0, limitOrder(Side::Buy, 4, "100", TimeInForce::Fok)),
  };
  const ReplayResult Res = core::replaySession(Spec, Tl, Journal, 1, 0);
  ASSERT_EQ(Res.Engine.orders().size(), 3u);
  EXPECT_EQ(Res.Engine.orders()[0].FilledLots, 0);
  EXPECT_EQ(Res.Engine.orders()[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(Res.Engine.orders()[0].ReasonCode, Reason::FillOrKill);

  EXPECT_EQ(Res.Engine.orders()[1].FilledLots, 0);
  EXPECT_EQ(Res.Engine.orders()[1].Status, OrderStatus::Cancelled);
  EXPECT_EQ(Res.Engine.orders()[1].ReasonCode, Reason::FillOrKill);

  EXPECT_EQ(Res.Engine.orders()[2].FilledLots, 4);
  EXPECT_EQ(Res.Engine.orders()[2].Status, OrderStatus::Filled);
  ASSERT_EQ(Res.Engine.fills().size(), 1u);
}

TEST(AdvancedOrders, IcebergMultiBarSlicing) {
  std::vector<Bar> Bars;
  Bars.push_back(makeBar(20240110, 10, "100", "100", 1000));
  Bars.push_back(makeBar(20240111, 10, "100", "100", 1000));
  Bars.push_back(makeBar(20240112, 10, "100", "100", 1000));
  Bars.push_back(makeBar(20240115, 10, "100", "100", 1000));
  Bars.push_back(makeBar(20240116, 10, "100", "100", 1000));
  const std::vector<std::int32_t> Dates = {
      20240110, 20240111, 20240112, 20240115, 20240116,
  };
  const auto Tl = makeTimeline(Bars, Dates);

  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs = CostConfig::zero();
  Spec.InitialCash = d("100000");

  OrderSpec Ice = limitOrder(Side::Buy, 10, "100", TimeInForce::Gtc);
  Ice.DisplayLots = 3;

  const std::vector<JournalEntry> Journal = {placeAt(0, 0, Ice)};
  const ReplayResult Res = core::replaySession(Spec, Tl, Journal, 4, 0);
  ASSERT_EQ(Res.Engine.fills().size(), 4u);
  EXPECT_EQ(Res.Engine.fills()[0].Lots, 3);
  EXPECT_EQ(Res.Engine.fills()[0].Step, 1);
  EXPECT_EQ(Res.Engine.fills()[1].Lots, 3);
  EXPECT_EQ(Res.Engine.fills()[1].Step, 2);
  EXPECT_EQ(Res.Engine.fills()[2].Lots, 3);
  EXPECT_EQ(Res.Engine.fills()[2].Step, 3);
  EXPECT_EQ(Res.Engine.fills()[3].Lots, 1);
  EXPECT_EQ(Res.Engine.fills()[3].Step, 4);
  EXPECT_EQ(Res.Engine.orders()[0].Status, OrderStatus::Filled);
  EXPECT_EQ(Res.Engine.portfolio().lots(0), 10);
}

TEST(AdvancedOrders, PriceBands10And40RejectAndClamp) {
  const std::vector<Bar> Bars = {
      makeBar(20240110, 10, "100", "100", 1000),
      makeBar(20240111, 10, "100", "100", 1000),
      makeBar(20240112, 10, "125", "125", 1000),
  };
  const auto Tl = makeTimeline(Bars, {20240110, 20240111, 20240112});

  // Планка ±10%: от prevClose = 100 коридор [90; 110].
  SessionSpec Spec10;
  Spec10.Instruments = Tl->instruments();
  Spec10.Costs = CostConfig::zero();
  Spec10.InitialCash = d("100000");
  Spec10.Rules.PriceBandPct = d("10");

  const std::vector<JournalEntry> Journal10 = {
      placeAt(0, 1, limitOrder(Side::Buy, 1, "89")),
      placeAt(1, 1, limitOrder(Side::Buy, 1, "111")),
      placeAt(2, 1, limitOrder(Side::Buy, 1, "90")),
      placeAt(3, 1, marketOrder(Side::Buy, 1)),
  };
  const ReplayResult Res10 = core::replaySession(Spec10, Tl, Journal10, 2, 0);
  ASSERT_EQ(Res10.CommandResults.size(), 4u);
  EXPECT_FALSE(Res10.CommandResults[0].Accepted);
  EXPECT_EQ(Res10.CommandResults[0].ReasonCode, Reason::PriceOutOfBand);
  EXPECT_EQ(Res10.CommandResults[0].Field, "limitPrice");
  EXPECT_FALSE(Res10.CommandResults[1].Accepted);
  EXPECT_EQ(Res10.CommandResults[1].ReasonCode, Reason::PriceOutOfBand);
  EXPECT_TRUE(Res10.CommandResults[2].Accepted);
  EXPECT_TRUE(Res10.CommandResults[3].Accepted);
  // Рыночная на гэпе 125 ограничивается верхней планкой 110.
  ASSERT_EQ(Res10.Engine.fills().size(), 1u);
  EXPECT_EQ(Res10.Engine.fills()[0].Price, d("110"));

  // Планка ±40%: от prevClose = 100 коридор [60; 140].
  SessionSpec Spec40 = Spec10;
  Spec40.Rules.PriceBandPct = d("40");
  const std::vector<JournalEntry> Journal40 = {
      placeAt(0, 1, limitOrder(Side::Buy, 1, "135")),
      placeAt(1, 1, limitOrder(Side::Buy, 1, "141")),
  };
  const ReplayResult Res40 = core::replaySession(Spec40, Tl, Journal40, 2, 0);
  EXPECT_TRUE(Res40.CommandResults[0].Accepted);
  EXPECT_FALSE(Res40.CommandResults[1].Accepted);
  EXPECT_EQ(Res40.CommandResults[1].ReasonCode, Reason::PriceOutOfBand);
}

TEST(AdvancedOrders, OpeningAndClosingAuctionsAndDeterminism) {
  // День 1: 3 часовых бара (шаг 0 - открытие, шаг 1 - обычный, шаг 2 -
  // закрытие). День 2: шаг 3 - открытие.
  const std::vector<Bar> Bars = {
      makeBar(20240110, 10, "100", "101", 1000),
      makeBar(20240110, 11, "101", "102", 1000),
      makeBar(20240110, 18, "102", "105", 1000),
      makeBar(20240111, 10, "104", "106", 1000),
  };
  const auto Tl = makeTimeline(Bars, {20240110, 20240111});

  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs.CommissionRate = Decimal::zero();
  Spec.Costs.ExchangeFeeRate = Decimal::zero();
  Spec.Costs.Spread = d("0.01");
  Spec.Costs.SlippageKappa = d("0.1");
  Spec.InitialCash = d("100000");
  Spec.Rules.Auction.EnableOpeningAuction = true;
  Spec.Rules.Auction.EnableClosingAuction = true;

  // 1) Рыночная AuctionOnly на шаге 0 -> пропускает шаг 1, исполняется на
  // аукционе закрытия шага 2 по close = 105 без спреда и проскальзывания.
  // 2) Лимитная AuctionOnly по 105 на шаге 2 -> исполняется на аукционе
  // открытия шага 3 по open = 104 без спреда и проскальзывания.
  OrderSpec CloseAuc = marketOrder(Side::Buy, 2);
  CloseAuc.AuctionOnly = true;
  OrderSpec OpenAuc = limitOrder(Side::Buy, 3, "105");
  OpenAuc.AuctionOnly = true;

  const std::vector<JournalEntry> Journal = {
      placeAt(0, 0, CloseAuc),
      placeAt(1, 2, OpenAuc),
  };

  const ReplayResult Res1 = core::replaySession(Spec, Tl, Journal, 3, 0);
  const ReplayResult Res2 = core::replaySession(Spec, Tl, Journal, 3, 0);

  ASSERT_EQ(Res1.Engine.fills().size(), 2u);
  EXPECT_EQ(Res1.Engine.fills()[0].Step, 2);
  EXPECT_EQ(Res1.Engine.fills()[0].Price, d("105"));
  EXPECT_EQ(Res1.Engine.fills()[0].SpreadCost, Decimal::zero());
  EXPECT_EQ(Res1.Engine.fills()[0].SlippageCost, Decimal::zero());

  EXPECT_EQ(Res1.Engine.fills()[1].Step, 3);
  EXPECT_EQ(Res1.Engine.fills()[1].Price, d("104"));
  EXPECT_EQ(Res1.Engine.fills()[1].SpreadCost, Decimal::zero());
  EXPECT_EQ(Res1.Engine.fills()[1].SlippageCost, Decimal::zero());

  // INV-4: детерминизм.
  EXPECT_EQ(core::digestOf(Res1.Engine), core::digestOf(Res2.Engine));
}

} // namespace
