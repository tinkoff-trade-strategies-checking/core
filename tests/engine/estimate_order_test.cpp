#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "core/blind/blind.hpp"
#include "core/engine/journal.hpp"
#include "core/engine/session_engine.hpp"
#include "core/exec/costs.hpp"
#include "core/exec/execution.hpp"
#include "core/market/bar.hpp"
#include "core/market/calendar_source.hpp"
#include "core/market/instrument.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"

namespace {

core::Bar makeBar(std::int64_t TsMs, const char *Open, const char *High,
                  const char *Low, const char *Close, std::int64_t Vol) {
  core::Bar B;
  B.TsOpenMs = TsMs;
  B.AvailableAtMs = TsMs + 60000;
  B.Open = core::Decimal::parse(Open);
  B.High = core::Decimal::parse(High);
  B.Low = core::Decimal::parse(Low);
  B.Close = core::Decimal::parse(Close);
  B.Volume = Vol;
  return B;
}

std::shared_ptr<core::Timeline> makeTimeline(const char *Close2 = "104.25") {
  std::vector<core::Instrument> Instruments = {
      {"A", "TKA", 10, core::Decimal::parse("0.01")},
  };
  const std::int64_t DayStart = 1736751600000LL;
  std::vector<std::vector<core::Bar>> Bars = {{
      makeBar(DayStart, "100", "102", "99", "101", 1000),
      makeBar(DayStart + 60000, "101", "105", "100", "103", 1000),
      makeBar(DayStart + 120000, "103", "106", "102", Close2, 1000),
      makeBar(DayStart + 180000, "200", "210", "190", "205", 1000),
  }};
  std::vector<core::TradingDay> Cal = {{20250113, true}};
  return std::make_shared<core::Timeline>(
      core::Timeline::build(Instruments, Bars, Cal));
}

core::SessionSpec makeSpec(const core::Decimal &InitialCash) {
  core::SessionSpec Spec;
  Spec.Instruments = {{"A", "TKA", 10, core::Decimal::parse("0.01")}};
  Spec.Costs.CommissionRate = core::Decimal::parse("0.003");
  Spec.Costs.ExchangeFeeRate = core::Decimal::parse("0.0001");
  Spec.Costs.Spread = core::Decimal::parse("0.001");
  Spec.Costs.SlippageKappa = core::Decimal::parse("0.1");
  Spec.InitialCash = InitialCash;
  return Spec;
}

} // namespace

TEST(EstimateOrder, MatchesPlacementMoneyCheckForMarketAndLimit) {
  auto Tl = makeTimeline();
  const core::Decimal Cash = core::Decimal::fromUnits(100000);
  const core::SessionSpec Spec = makeSpec(Cash);

  core::OrderSpec MarketBuy;
  MarketBuy.Instrument = 0;
  MarketBuy.OrderSide = core::Side::Buy;
  MarketBuy.Type = core::OrderType::Market;
  MarketBuy.Lots = 5;
  MarketBuy.Tif = core::TimeInForce::Day;

  const core::OrderEstimate MarketEst =
      core::estimateOrder(Spec, Tl, {}, 2, MarketBuy);
  EXPECT_TRUE(MarketEst.Check.Ok);
  EXPECT_EQ(MarketEst.ReferencePrice, core::Decimal::parse("104.25"));

  const core::Amounts ExpectedAmounts =
      core::amounts(MarketEst.ReferencePrice, 5, 10, Spec.Costs);
  EXPECT_EQ(MarketEst.Value, ExpectedAmounts.Value);
  EXPECT_EQ(MarketEst.Commission, ExpectedAmounts.Commission);
  EXPECT_EQ(MarketEst.Fee, ExpectedAmounts.Fee);
  EXPECT_EQ(MarketEst.ValuePct,
            core::toPercentOfCapital(ExpectedAmounts.Value, Cash));
  EXPECT_EQ(MarketEst.CommissionPct,
            core::toPercentOfCapital(ExpectedAmounts.Commission, Cash));
  EXPECT_EQ(MarketEst.FeePct,
            core::toPercentOfCapital(ExpectedAmounts.Fee, Cash));
  EXPECT_EQ(MarketEst.RequiredCashPct,
            core::toPercentOfCapital(MarketEst.RequiredCash, Cash));

  // Лимитная заявка с ценой ниже close проверяется по close последнего бара.
  core::OrderSpec LimitBuy = MarketBuy;
  LimitBuy.Type = core::OrderType::Limit;
  LimitBuy.LimitPrice = core::Decimal::parse("90.00");
  const core::OrderEstimate LimitEst =
      core::estimateOrder(Spec, Tl, {}, 2, LimitBuy);
  EXPECT_TRUE(LimitEst.Check.Ok);
  EXPECT_EQ(LimitEst.ReferencePrice, MarketEst.ReferencePrice);
  EXPECT_EQ(LimitEst.Value, MarketEst.Value);
  EXPECT_EQ(LimitEst.Commission, MarketEst.Commission);
  EXPECT_EQ(LimitEst.Fee, MarketEst.Fee);
  EXPECT_EQ(LimitEst.RequiredCash, MarketEst.RequiredCash);

  // Точная граница проверки денег при подаче (validate): при cash ==
  // RequiredCash заявка проходит, на 1 наноединицу меньше — InsufficientCash.
  core::ExecutionEngine Exec(Spec.Instruments, Spec.Costs);
  core::Portfolio ExactPortfolio(MarketEst.RequiredCash, 1);
  EXPECT_TRUE(Exec.validate(MarketBuy, ExactPortfolio, *Tl, 2).Ok);
  EXPECT_TRUE(Exec.validate(LimitBuy, ExactPortfolio, *Tl, 2).Ok);

  const core::Decimal OneNano = core::Decimal::fromRaw(1);
  core::Portfolio ShortPortfolio(MarketEst.RequiredCash - OneNano, 1);
  EXPECT_EQ(Exec.validate(MarketBuy, ShortPortfolio, *Tl, 2).ReasonCode,
            core::Reason::InsufficientCash);
  EXPECT_EQ(Exec.validate(LimitBuy, ShortPortfolio, *Tl, 2).ReasonCode,
            core::Reason::InsufficientCash);
}

TEST(EstimateOrder, UsesJournalStateWithoutMutatingAndChecksInsufficientCash) {
  auto Tl = makeTimeline();
  const core::Decimal Cash = core::Decimal::fromUnits(6000);
  const core::SessionSpec Spec = makeSpec(Cash);

  core::PlaceCmd BuyCmd;
  BuyCmd.Order.Instrument = 0;
  BuyCmd.Order.OrderSide = core::Side::Buy;
  BuyCmd.Order.Type = core::OrderType::Market;
  BuyCmd.Order.Lots = 4;
  BuyCmd.Order.Tif = core::TimeInForce::Gtc;
  const std::vector<core::JournalEntry> Journal = {{0, 0, BuyCmd}};

  // После покупки 4 лотов на шаге 1 денег осталось ~1940: ещё 3 лота не
  // хватает.
  core::OrderSpec BuyMore = BuyCmd.Order;
  BuyMore.Lots = 3;
  const core::OrderEstimate BuyEst =
      core::estimateOrder(Spec, Tl, Journal, 2, BuyMore);
  EXPECT_FALSE(BuyEst.Check.Ok);
  EXPECT_EQ(BuyEst.Check.ReasonCode, core::Reason::InsufficientCash);
  EXPECT_GT(BuyEst.Value, core::Decimal::zero());
  EXPECT_GT(BuyEst.RequiredCash, core::Decimal::zero());

  // Продажа 4 лотов (из имеющихся 4) проходит, 5 лотов — InsufficientPosition.
  core::OrderSpec SellOk = BuyCmd.Order;
  SellOk.OrderSide = core::Side::Sell;
  SellOk.Lots = 4;
  EXPECT_TRUE(core::estimateOrder(Spec, Tl, Journal, 2, SellOk).Check.Ok);

  core::OrderSpec SellTooMuch = SellOk;
  SellTooMuch.Lots = 5;
  const core::OrderEstimate SellEst =
      core::estimateOrder(Spec, Tl, Journal, 2, SellTooMuch);
  EXPECT_FALSE(SellEst.Check.Ok);
  EXPECT_EQ(SellEst.Check.ReasonCode, core::Reason::InsufficientPosition);
}

TEST(EstimateOrder, NoFutureBarsAffectEstimate) {
  auto Tl1 = makeTimeline("104.25");
  std::vector<core::Instrument> Instruments = {
      {"A", "TKA", 10, core::Decimal::parse("0.01")},
  };
  const std::int64_t DayStart = 1736751600000LL;
  auto Tl2 = std::make_shared<core::Timeline>(core::Timeline::build(
      Instruments,
      {{
          makeBar(DayStart, "100", "102", "99", "101", 1000),
          makeBar(DayStart + 60000, "101", "105", "100", "103", 1000),
          makeBar(DayStart + 120000, "103", "106", "102", "104.25", 1000),
          makeBar(DayStart + 180000, "999", "999", "999", "999", 999999),
      }},
      {{20250113, true}}));

  const core::SessionSpec Spec = makeSpec(core::Decimal::fromUnits(100000));
  core::OrderSpec Order;
  Order.Instrument = 0;
  Order.OrderSide = core::Side::Buy;
  Order.Type = core::OrderType::Market;
  Order.Lots = 5;
  Order.Tif = core::TimeInForce::Day;

  const core::OrderEstimate E1 = core::estimateOrder(Spec, Tl1, {}, 2, Order);
  const core::OrderEstimate E2 = core::estimateOrder(Spec, Tl2, {}, 2, Order);
  EXPECT_EQ(E1.Check.Ok, E2.Check.Ok);
  EXPECT_EQ(E1.ReferencePrice, E2.ReferencePrice);
  EXPECT_EQ(E1.Value, E2.Value);
  EXPECT_EQ(E1.Commission, E2.Commission);
  EXPECT_EQ(E1.Fee, E2.Fee);
  EXPECT_EQ(E1.RequiredCash, E2.RequiredCash);
}
