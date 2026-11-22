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
  // required_cash округляется вверх до копейки (CEILING, 2 знака):
  // 104.25 * 5 * 10 * (1 + 0.003 + 0.0001) = 5228.65875 -> 5228.66.
  EXPECT_EQ(MarketEst.RequiredCash, core::Decimal::parse("5228.66"));
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

  // Проверка денег при подаче (validate) не меняется и использует
  // неокруглённый requiredCashForOrder; в ответе оценки RequiredCash
  // округлён вверх до копейки (верхняя граница резерва >= порога validate).
  core::ExecutionEngine Exec(Spec.Instruments, Spec.Costs);
  core::Portfolio ExactPortfolio(MarketEst.RequiredCash, 1);
  EXPECT_TRUE(Exec.validate(MarketBuy, ExactPortfolio, *Tl, 2).Ok);
  EXPECT_TRUE(Exec.validate(LimitBuy, ExactPortfolio, *Tl, 2).Ok);

  const core::Decimal ExactNeed =
      core::requiredCashForOrder(MarketEst.ReferencePrice, 5, 10, Spec.Costs);
  EXPECT_GE(MarketEst.RequiredCash, ExactNeed);
  const core::Decimal OneNano = core::Decimal::fromRaw(1);
  core::Portfolio ShortPortfolio(ExactNeed - OneNano, 1);
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
  const std::vector<core::JournalEntry> Journal = {{0, 0, BuyCmd, "", ""}};

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

TEST(EstimateOrder, ReturnsInsufficientCashOnDecimalOverflow) {
  auto Tl = makeTimeline("1000000000000000"); // 10^15
  const core::SessionSpec Spec = makeSpec(core::Decimal::fromUnits(100000));
  core::OrderSpec Order;
  Order.Instrument = 0;
  Order.OrderSide = core::Side::Buy;
  Order.Type = core::OrderType::Market;
  Order.Lots =
      9223372036854775807LL; // 10^15 * 9.22*10^18 * 10 > 1.7*10^29 -> overflow
  Order.Tif = core::TimeInForce::Day;
  const core::OrderEstimate Est = core::estimateOrder(Spec, Tl, {}, 2, Order);
  EXPECT_FALSE(Est.Check.Ok);
  EXPECT_EQ(Est.Check.ReasonCode, core::Reason::InsufficientCash);
  EXPECT_EQ(Est.Check.Field, "");
}

TEST(EstimateOrder, MatchesSubmissionRejectionReasons) {
  std::vector<core::Instrument> Instruments = {
      {"A", "FIXA", 10, core::Decimal::parse("0.01")},
      {"B", "FIXB", 10, core::Decimal::parse("0.01")},
  };
  const std::int64_t DayStart = 1736751600000LL;
  auto Tl = std::make_shared<core::Timeline>(core::Timeline::build(
      Instruments,
      {
          {
              makeBar(DayStart, "100", "105", "95", "100", 1000),
              makeBar(DayStart + 60000, "100", "105", "95", "100", 1000),
              makeBar(DayStart + 120000, "100", "105", "95", "100", 1000),
          },
          {
              // У B нет бара на шаге 0.
              makeBar(DayStart + 60000, "200", "205", "195", "200", 1000),
              makeBar(DayStart + 120000, "200", "205", "195", "200", 1000),
          },
      },
      {{20250113, true}}));

  core::SessionSpec Spec = makeSpec(core::Decimal::fromUnits(10000));
  Spec.Instruments = Instruments;

  auto CheckSame = [&](std::int64_t Step, const core::OrderSpec &Order) {
    const core::OrderEstimate Est =
        core::estimateOrder(Spec, Tl, {}, Step, Order);
    core::SessionEngine Engine(Spec, Tl);
    for (std::int64_t S = 0; S < Step; ++S)
      Engine.advance();
    core::JournalEntry Entry;
    Entry.Seq = 0;
    Entry.Step = Step;
    Entry.Command = core::PlaceCmd{Order, std::nullopt};
    const core::CommandResult Res = Engine.apply(Entry);
    EXPECT_EQ(Est.Check.Ok, Res.Accepted);
    EXPECT_EQ(Est.Check.ReasonCode, Res.ReasonCode);
    EXPECT_EQ(Est.Check.Field, Res.Field);
  };

  core::OrderSpec BadInst;
  BadInst.Instrument = 5;
  BadInst.OrderSide = core::Side::Buy;
  BadInst.Type = core::OrderType::Market;
  BadInst.Lots = 1;
  CheckSame(0, BadInst);

  core::OrderSpec ZeroLots = BadInst;
  ZeroLots.Instrument = 0;
  ZeroLots.Lots = 0;
  CheckSame(0, ZeroLots);

  core::OrderSpec OffGrid = ZeroLots;
  OffGrid.Lots = 1;
  OffGrid.Type = core::OrderType::Limit;
  OffGrid.LimitPrice = core::Decimal::parse("99.005");
  CheckSame(0, OffGrid);

  core::OrderSpec NoBar = ZeroLots;
  NoBar.Instrument = 1;
  NoBar.Lots = 1;
  CheckSame(0, NoBar);

  core::OrderSpec HugeLots = ZeroLots;
  HugeLots.Lots = 9223372036854775807LL;
  CheckSame(0, HugeLots);

  core::OrderSpec ShortSell = ZeroLots;
  ShortSell.OrderSide = core::Side::Sell;
  ShortSell.Lots = 1;
  CheckSame(0, ShortSell);

  // Остановленная сессия на последнем шаге (шаг 2).
  CheckSame(2, ZeroLots);
}

TEST(EstimateOrder, AcceptsBlindPriceAndConvertsToRubles) {
  auto Tl = makeTimeline("200");
  const core::SessionSpec Spec = makeSpec(core::Decimal::fromUnits(100000));

  core::OrderSpec Order;
  Order.Instrument = 0;
  Order.OrderSide = core::Side::Buy;
  Order.Type = core::OrderType::Limit;
  Order.Lots = 5;
  Order.LimitPrice = core::Decimal::parse("100.23"); // P0 = 100 -> 100.23 ₽
  Order.IsBlindPrice = true;
  Order.Tif = core::TimeInForce::Day;

  const core::OrderEstimate Est = core::estimateOrder(Spec, Tl, {}, 2, Order);
  EXPECT_TRUE(Est.Check.Ok);
  EXPECT_EQ(Est.ReferencePrice, core::Decimal::parse("200"));
  EXPECT_GT(Est.Value, core::Decimal::zero());
  EXPECT_GT(Est.RequiredCash, core::Decimal::zero());
  EXPECT_GT(Est.ValuePct, core::Decimal::zero());
  ASSERT_TRUE(Est.ResolvedOrder.has_value());
  EXPECT_FALSE(Est.ResolvedOrder->IsBlindPrice);
  EXPECT_EQ(*Est.ResolvedOrder->LimitPrice, core::Decimal::parse("100.23"));
}

TEST(EstimateOrder, ConvertsBlindPricesAndRejectsWithoutP0AndBadTrailing) {
  std::vector<core::Instrument> Instruments = {
      {"A", "FIXA", 10, core::Decimal::parse("0.05")},
      {"B", "FIXB", 10, core::Decimal::parse("0.05")},
  };
  const std::int64_t DayStart = 1736751600000LL;
  auto Tl = std::make_shared<core::Timeline>(core::Timeline::build(
      Instruments,
      {
          {
              makeBar(DayStart, "200", "210", "195", "205", 1000),
              makeBar(DayStart + 60000, "205", "215", "200", "210", 1000),
              makeBar(DayStart + 120000, "210", "220", "205", "215", 1000),
          },
          {
              // У B нет бара на шаге 0; первый бар на шаге 1.
              makeBar(DayStart + 60000, "500", "510", "490", "505", 1000),
              makeBar(DayStart + 120000, "505", "515", "495", "510", 1000),
          },
      },
      {{20250113, true}}));

  core::SessionSpec Spec;
  Spec.Instruments = Instruments;
  Spec.Costs = core::CostConfig::defaults();
  Spec.InitialCash = core::Decimal::fromUnits(100000);

  // Стоп на покупку по A: close на шаге 1 = 210, P0 = 200 -> 105.00 в слепой
  // шкале. 104.00 -> 208.00 <= 210 -> отказ по stopPrice, но ResolvedOrder
  // всё равно содержит переведённую рублёвую заявку.
  core::OrderSpec BuyStop;
  BuyStop.Instrument = 0;
  BuyStop.OrderSide = core::Side::Buy;
  BuyStop.Type = core::OrderType::Stop;
  BuyStop.Lots = 1;
  BuyStop.StopPrice = core::Decimal::parse("104.00");
  BuyStop.IsBlindPrice = true;
  const core::OrderEstimate BadSide =
      core::estimateOrder(Spec, Tl, {}, 1, BuyStop);
  EXPECT_FALSE(BadSide.Check.Ok);
  EXPECT_EQ(BadSide.Check.ReasonCode, core::Reason::InvalidParams);
  EXPECT_EQ(BadSide.Check.Field, "stopPrice");
  ASSERT_TRUE(BadSide.ResolvedOrder.has_value());
  EXPECT_FALSE(BadSide.ResolvedOrder->IsBlindPrice);
  EXPECT_EQ(*BadSide.ResolvedOrder->StopPrice, core::Decimal::parse("208.00"));

  // 106.00 -> 212.00 > 210 -> допустимо.
  BuyStop.StopPrice = core::Decimal::parse("106.00");
  const core::OrderEstimate Good =
      core::estimateOrder(Spec, Tl, {}, 1, BuyStop);
  EXPECT_TRUE(Good.Check.Ok);
  ASSERT_TRUE(Good.ResolvedOrder.has_value());
  EXPECT_FALSE(Good.ResolvedOrder->IsBlindPrice);
  EXPECT_EQ(*Good.ResolvedOrder->StopPrice, core::Decimal::parse("212.00"));

  // По бумаге B на шаге 0 нет P0 -> can_place = false,
  // InvalidParams/instrument.
  core::OrderSpec BlindOnB;
  BlindOnB.Instrument = 1;
  BlindOnB.OrderSide = core::Side::Buy;
  BlindOnB.Type = core::OrderType::Limit;
  BlindOnB.Lots = 1;
  BlindOnB.LimitPrice = core::Decimal::parse("100.00");
  BlindOnB.IsBlindPrice = true;
  const core::OrderEstimate NoP0 =
      core::estimateOrder(Spec, Tl, {}, 0, BlindOnB);
  EXPECT_FALSE(NoP0.Check.Ok);
  EXPECT_EQ(NoP0.Check.ReasonCode, core::Reason::InvalidParams);
  EXPECT_EQ(NoP0.Check.Field, "instrument");
  EXPECT_FALSE(NoP0.ResolvedOrder.has_value());

  const core::OrderEstimate AtStep1 =
      core::estimateOrder(Spec, Tl, {}, 1, BlindOnB);
  EXPECT_TRUE(AtStep1.Check.Ok);
  ASSERT_TRUE(AtStep1.ResolvedOrder.has_value());
  EXPECT_EQ(*AtStep1.ResolvedOrder->LimitPrice, core::Decimal::parse("500.00"));

  // Неверный trailing_percent у слепой заявки не проходит молча.
  core::OrderSpec BadTrailing;
  BadTrailing.Instrument = 0;
  BadTrailing.OrderSide = core::Side::Sell;
  BadTrailing.Type = core::OrderType::TrailingStop;
  BadTrailing.Lots = 1;
  BadTrailing.TrailingPercent = core::Decimal::parse("60");
  BadTrailing.IsBlindPrice = true;
  const core::OrderEstimate TrailingRes =
      core::estimateOrder(Spec, Tl, {}, 1, BadTrailing);
  EXPECT_FALSE(TrailingRes.Check.Ok);
  EXPECT_EQ(TrailingRes.Check.ReasonCode, core::Reason::InvalidParams);
  EXPECT_EQ(TrailingRes.Check.Field, "trailingPercent");
}
