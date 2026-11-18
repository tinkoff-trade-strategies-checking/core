#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "core/exec/execution.hpp"
#include "core/market/timeline.hpp"

namespace {

using core::Bar;
using core::CostConfig;
using core::Decimal;
using core::ExecutionEngine;
using core::Instrument;
using core::OrderSpec;
using core::OrderType;
using core::Portfolio;
using core::Reason;
using core::Side;
using core::Timeline;
using core::Validation;

Decimal d(const char *Text) { return Decimal::parse(Text); }

constexpr std::int64_t kMinuteMs = 60'000;
constexpr std::int64_t kStart = 1'704'747'600'000 + 10 * 3'600'000;

const Instrument kA{"A", "FIXA", 10, Decimal::parse("0.01")};
const Instrument kB{"B", "FIXB", 10000, Decimal::parse("0.000005")};

Bar bar(int Minute, const char *Price) {
  Bar B;
  B.TsOpenMs = kStart + Minute * kMinuteMs;
  B.AvailableAtMs = B.TsOpenMs + kMinuteMs;
  B.Open = B.High = B.Low = B.Close = d(Price);
  B.Volume = 100000;
  return B;
}

// A: close 100.00 на шагах 0…2; B: первый бар на шаге 2 (до этого цены нет),
// close 0.0200.
struct Fixture {
  Timeline Line = Timeline::build(
      {kA, kB},
      {{bar(0, "100"), bar(1, "100"), bar(2, "100")}, {bar(2, "0.0200")}}, {});
  ExecutionEngine Engine{{kA, kB}, CostConfig::defaults()};
  Portfolio Folio{d("1000000"), 2};

  Validation check(const OrderSpec &Spec, std::int64_t Step = 2) const {
    return Engine.validate(Spec, Folio, Line, Step);
  }
  void hold(std::int64_t Lots) {
    Folio.applyFill(0, Side::Buy, Lots, d("100"), d("5000.00"), d("2.50"),
                    d("0.50"), 0, 1);
  }
};

OrderSpec spec(Side Side, OrderType Type, std::int64_t Lots,
               int Instrument = 0) {
  OrderSpec S;
  S.Instrument = Instrument;
  S.OrderSide = Side;
  S.Type = Type;
  S.Lots = Lots;
  return S;
}

void expectInvalid(const Validation &V, const char *Field) {
  EXPECT_FALSE(V.Ok);
  EXPECT_EQ(V.ReasonCode, Reason::InvalidParams);
  EXPECT_EQ(V.Field, Field);
}

TEST(Validation, ValidOrdersPass) {
  Fixture F;
  EXPECT_TRUE(F.check(spec(Side::Buy, OrderType::Market, 10)).Ok);
  OrderSpec Limit = spec(Side::Buy, OrderType::Limit, 10);
  Limit.LimitPrice = d("99");
  EXPECT_TRUE(F.check(Limit).Ok);
  OrderSpec Stop = spec(Side::Buy, OrderType::Stop, 10);
  Stop.StopPrice = d("101");
  EXPECT_TRUE(F.check(Stop).Ok);
  OrderSpec StopLimit = spec(Side::Buy, OrderType::StopLimit, 10);
  StopLimit.StopPrice = d("101");
  StopLimit.LimitPrice = d("102");
  EXPECT_TRUE(F.check(StopLimit).Ok);
}

TEST(Validation, StopWrongSide) {
  Fixture F;
  // Стоп на продажу должен быть ниже последнего close (100).
  OrderSpec SellStop = spec(Side::Sell, OrderType::Stop, 1);
  SellStop.StopPrice = d("101");
  expectInvalid(F.check(SellStop), "stopPrice");
  // Стоп на покупку - выше.
  OrderSpec BuyStop = spec(Side::Buy, OrderType::Stop, 1);
  BuyStop.StopPrice = d("99");
  expectInvalid(F.check(BuyStop), "stopPrice");
}

TEST(Validation, StopAtCloseIsWrongSide) {
  Fixture F;
  OrderSpec BuyStop = spec(Side::Buy, OrderType::Stop, 1);
  BuyStop.StopPrice = d("100");
  expectInvalid(F.check(BuyStop), "stopPrice");
}

TEST(Validation, SellStopBelowCloseWithPosition) {
  Fixture F;
  F.hold(5);
  OrderSpec SellStop = spec(Side::Sell, OrderType::Stop, 5);
  SellStop.StopPrice = d("95");
  EXPECT_TRUE(F.check(SellStop).Ok);
}

TEST(Validation, LotsMustBePositive) {
  Fixture F;
  expectInvalid(F.check(spec(Side::Buy, OrderType::Market, 0)), "lots");
  expectInvalid(F.check(spec(Side::Buy, OrderType::Market, -3)), "lots");
}

TEST(Validation, PricesMustBePositiveAndPresent) {
  Fixture F;
  OrderSpec Limit = spec(Side::Buy, OrderType::Limit, 1);
  expectInvalid(F.check(Limit), "limitPrice");
  Limit.LimitPrice = d("0");
  expectInvalid(F.check(Limit), "limitPrice");
  Limit.LimitPrice = d("-1");
  expectInvalid(F.check(Limit), "limitPrice");

  OrderSpec Stop = spec(Side::Buy, OrderType::Stop, 1);
  expectInvalid(F.check(Stop), "stopPrice");
  Stop.StopPrice = d("0");
  expectInvalid(F.check(Stop), "stopPrice");

  OrderSpec StopLimit = spec(Side::Buy, OrderType::StopLimit, 1);
  StopLimit.StopPrice = d("101");
  expectInvalid(F.check(StopLimit), "limitPrice");
}

TEST(Validation, PricesMustBeMultipleOfPriceStep) {
  Fixture F;
  OrderSpec Limit = spec(Side::Buy, OrderType::Limit, 1);
  Limit.LimitPrice = d("99.99");
  EXPECT_TRUE(F.check(Limit).Ok);
  Limit.LimitPrice = d("99.995");
  expectInvalid(F.check(Limit), "limitPrice");

  OrderSpec Stop = spec(Side::Buy, OrderType::Stop, 1);
  Stop.StopPrice = d("101.01");
  EXPECT_TRUE(F.check(Stop).Ok);
  Stop.StopPrice = d("101.005");
  expectInvalid(F.check(Stop), "stopPrice");

  OrderSpec StopLimit = spec(Side::Buy, OrderType::StopLimit, 1);
  StopLimit.StopPrice = d("101");
  StopLimit.LimitPrice = d("102.001");
  expectInvalid(F.check(StopLimit), "limitPrice");

  OrderSpec Take = spec(Side::Sell, OrderType::TakeProfit, 1);
  F.hold(1);
  Take.LimitPrice = d("101.5");
  EXPECT_TRUE(F.check(Take).Ok);
  Take.LimitPrice = d("101.505");
  expectInvalid(F.check(Take), "limitPrice");
}

TEST(Validation, PriceStepOfSecurityBIsRespected) {
  Fixture F;
  OrderSpec Limit = spec(Side::Buy, OrderType::Limit, 1, 1);
  Limit.LimitPrice = d("0.019995");
  EXPECT_TRUE(F.check(Limit).Ok);
  Limit.LimitPrice = d("0.0199951");
  expectInvalid(F.check(Limit), "limitPrice");
}

TEST(Validation, TrailingPercentRange) {
  Fixture F;
  F.hold(5);
  OrderSpec Trailing = spec(Side::Sell, OrderType::TrailingStop, 5);
  expectInvalid(F.check(Trailing), "trailingPercent");
  Trailing.TrailingPercent = d("0");
  expectInvalid(F.check(Trailing), "trailingPercent");
  Trailing.TrailingPercent = d("50.000000001");
  expectInvalid(F.check(Trailing), "trailingPercent");
  Trailing.TrailingPercent = d("50");
  EXPECT_TRUE(F.check(Trailing).Ok);
  Trailing.TrailingPercent = d("0.000000001");
  EXPECT_TRUE(F.check(Trailing).Ok);
}

TEST(Validation, TakeProfitAndTrailingAreSellOnly) {
  Fixture F;
  OrderSpec Take = spec(Side::Buy, OrderType::TakeProfit, 1);
  Take.LimitPrice = d("110");
  expectInvalid(F.check(Take), "side");
  OrderSpec Trailing = spec(Side::Buy, OrderType::TrailingStop, 1);
  Trailing.TrailingPercent = d("5");
  expectInvalid(F.check(Trailing), "side");
}

TEST(Validation, TakeProfitMustBeAboveClose) {
  Fixture F;
  F.hold(5);
  OrderSpec Take = spec(Side::Sell, OrderType::TakeProfit, 5);
  Take.LimitPrice = d("99");
  expectInvalid(F.check(Take), "limitPrice");
  Take.LimitPrice = d("110");
  EXPECT_TRUE(F.check(Take).Ok);
}

TEST(Validation, InsufficientCashUsesLastCloseWithCommissionAndFee) {
  // 100 лотов * 10 * 100 = 100 000; с комиссией и сбором 100 000 * 1.0031 = 100
  // 310.
  Fixture F;
  const OrderSpec Order = spec(Side::Buy, OrderType::Market, 100);
  Portfolio Enough(d("100310"), 2);
  EXPECT_TRUE(F.Engine.validate(Order, Enough, F.Line, 2).Ok);
  Portfolio NotEnough(d("100309.99"), 2);
  const Validation V = F.Engine.validate(Order, NotEnough, F.Line, 2);
  EXPECT_FALSE(V.Ok);
  EXPECT_EQ(V.ReasonCode, Reason::InsufficientCash);
}

TEST(Validation, CashCheckAlsoForLimitOrdersAtLastClose) {
  Fixture F;
  OrderSpec Limit =
      spec(Side::Buy, OrderType::Limit, 100000); // 100 000 000 ₽ при close 100
  Limit.LimitPrice = d("1");
  const Validation V = F.check(Limit);
  EXPECT_FALSE(V.Ok);
  EXPECT_EQ(V.ReasonCode, Reason::InsufficientCash);
}

TEST(Validation, HugeOrderIsInsufficientCashNotOverflow) {
  Fixture F;
  const Validation V = F.check(spec(Side::Buy, OrderType::Market,
                                    std::numeric_limits<std::int64_t>::max()));
  EXPECT_FALSE(V.Ok);
  EXPECT_EQ(V.ReasonCode, Reason::InsufficientCash);
}

TEST(Validation, InsufficientPosition) {
  Fixture F;
  F.hold(5);
  EXPECT_TRUE(F.check(spec(Side::Sell, OrderType::Market, 5)).Ok);
  const Validation V = F.check(spec(Side::Sell, OrderType::Market, 6));
  EXPECT_FALSE(V.Ok);
  EXPECT_EQ(V.ReasonCode, Reason::InsufficientPosition);
  EXPECT_TRUE(V.Field.empty());
}

TEST(Validation, UnknownOrUntradedInstrument) {
  Fixture F;
  expectInvalid(F.check(spec(Side::Buy, OrderType::Market, 1, 2)),
                "instrument");
  expectInvalid(F.check(spec(Side::Buy, OrderType::Market, 1, -1)),
                "instrument");
  // У B на шаге 1 ещё не было баров - цены для проверки нет.
  expectInvalid(F.check(spec(Side::Buy, OrderType::Market, 1, 1), 1),
                "instrument");
  EXPECT_TRUE(F.check(spec(Side::Buy, OrderType::Market, 1, 1), 2).Ok);
}

TEST(Validation, UsesLastCloseOfOwnInstrument) {
  // Цена B - 0.0200 (бар шага 2): 1 лот = 10 000 штук = 200 ₽, с издержками
  // 200.62.
  Fixture F;
  const OrderSpec Order = spec(Side::Buy, OrderType::Market, 1, 1);
  Portfolio Poor(d("200.61"), 2);
  EXPECT_EQ(F.Engine.validate(Order, Poor, F.Line, 2).ReasonCode,
            Reason::InsufficientCash);
  Portfolio Enough(d("200.62"), 2);
  EXPECT_TRUE(F.Engine.validate(Order, Enough, F.Line, 2).Ok);
}

TEST(OcoValidation, ValidPairsPass) {
  // Stop + TakeProfit на одной бумаге и стороне
  OrderSpec Stop = spec(Side::Sell, OrderType::Stop, 5, 0);
  Stop.StopPrice = d("90");
  OrderSpec Take = spec(Side::Sell, OrderType::TakeProfit, 5, 0);
  Take.LimitPrice = d("110");
  EXPECT_TRUE(core::validateOcoPair(Stop, Take).Ok);
  // Обратный порядок ног: TakeProfit + Stop
  EXPECT_TRUE(core::validateOcoPair(Take, Stop).Ok);

  // StopLimit + TakeProfit
  OrderSpec StopLimit = spec(Side::Sell, OrderType::StopLimit, 5, 0);
  StopLimit.StopPrice = d("90");
  StopLimit.LimitPrice = d("89");
  EXPECT_TRUE(core::validateOcoPair(StopLimit, Take).Ok);
  EXPECT_TRUE(core::validateOcoPair(Take, StopLimit).Ok);
}

TEST(OcoValidation, StopLimitPlusTakeIsAccepted) {
  // OCO «стоп + тейк»: стоп = стоп-рыночная или стоп-лимитная (decisions.md).
  Fixture F;
  F.hold(5);
  OrderSpec StopLimit = spec(Side::Sell, OrderType::StopLimit, 5, 0);
  StopLimit.StopPrice = d("90");
  StopLimit.LimitPrice = d("89");
  OrderSpec Take = spec(Side::Sell, OrderType::TakeProfit, 5, 0);
  Take.LimitPrice = d("110");
  EXPECT_TRUE(F.check(StopLimit).Ok);
  EXPECT_TRUE(F.check(Take).Ok);
  EXPECT_TRUE(core::validateOcoPair(StopLimit, Take).Ok);
  EXPECT_TRUE(core::validateOcoPair(Take, StopLimit).Ok);
}

TEST(OcoValidation, RejectsDifferentInstruments) {
  OrderSpec Stop = spec(Side::Sell, OrderType::Stop, 5, 0);
  Stop.StopPrice = d("90");
  OrderSpec Take = spec(Side::Sell, OrderType::TakeProfit, 5, 1);
  Take.LimitPrice = d("110");
  expectInvalid(core::validateOcoPair(Stop, Take), "instrument");
}

TEST(OcoValidation, RejectsDifferentSides) {
  OrderSpec Stop = spec(Side::Sell, OrderType::Stop, 5, 0);
  Stop.StopPrice = d("90");
  OrderSpec Take = spec(Side::Buy, OrderType::TakeProfit, 5, 0);
  Take.LimitPrice = d("110");
  expectInvalid(core::validateOcoPair(Stop, Take), "side");
}

TEST(OcoValidation, RejectsBuyPair) {
  // Тейк-профит - только продажа, поэтому пара на покупку невозможна.
  OrderSpec Stop = spec(Side::Buy, OrderType::Stop, 5, 0);
  Stop.StopPrice = d("110");
  OrderSpec Take = spec(Side::Buy, OrderType::TakeProfit, 5, 0);
  Take.LimitPrice = d("90");
  expectInvalid(core::validateOcoPair(Stop, Take), "side");
  expectInvalid(core::validateOcoPair(Take, Stop), "side");
}

TEST(OcoValidation, RejectsInvalidTypes) {
  OrderSpec Stop = spec(Side::Sell, OrderType::Stop, 5, 0);
  Stop.StopPrice = d("90");
  OrderSpec Take = spec(Side::Sell, OrderType::TakeProfit, 5, 0);
  Take.LimitPrice = d("110");
  OrderSpec Limit = spec(Side::Sell, OrderType::Limit, 5, 0);
  Limit.LimitPrice = d("105");
  OrderSpec Market = spec(Side::Sell, OrderType::Market, 5, 0);
  OrderSpec Trailing = spec(Side::Sell, OrderType::TrailingStop, 5, 0);
  Trailing.TrailingPercent = d("5");

  // Лимит + тейк-профит
  expectInvalid(core::validateOcoPair(Limit, Take), "type");
  // Стоп + стоп
  expectInvalid(core::validateOcoPair(Stop, Stop), "type");
  // Тейк + тейк
  expectInvalid(core::validateOcoPair(Take, Take), "type");
  // Рыночная + стоп
  expectInvalid(core::validateOcoPair(Market, Stop), "type");
  // Трейлинг + тейк
  expectInvalid(core::validateOcoPair(Trailing, Take), "type");
}

TEST(Validation, RejectsBlindPricesInJournal) {
  Fixture F;
  OrderSpec S = spec(Side::Buy, OrderType::Limit, 1);
  S.LimitPrice = d("100");
  S.BlindPrices = true;
  expectInvalid(F.check(S), "blindPrices");
}

TEST(Validation, RejectsNonPositivePriceStep) {
  const Instrument ZeroStep{"A", "FIXA", 10, Decimal::zero()};
  Timeline Line = Timeline::build({ZeroStep}, {{bar(0, "100")}}, {});
  ExecutionEngine Exec({ZeroStep}, CostConfig{});
  Portfolio Folio(d("100000"), 1);
  OrderSpec S = spec(Side::Buy, OrderType::Market, 1);
  expectInvalid(Exec.validate(S, Folio, Line, 0), "instrument");
}

} // namespace
