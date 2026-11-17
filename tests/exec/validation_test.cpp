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

Decimal D(const char *text) { return Decimal::parse(text); }

constexpr std::int64_t kMinuteMs = 60'000;
constexpr std::int64_t kStart = 1'704'747'600'000 + 10 * 3'600'000;

const Instrument kA{"A", "FIXA", 10, Decimal::parse("0.01")};
const Instrument kB{"B", "FIXB", 10000, Decimal::parse("0.000005")};

Bar bar(int minute, const char *price) {
  Bar b;
  b.tsOpenMs = kStart + minute * kMinuteMs;
  b.availableAtMs = b.tsOpenMs + kMinuteMs;
  b.open = b.high = b.low = b.close = D(price);
  b.volume = 100000;
  return b;
}

// A: close 100.00 на шагах 0…2; B: первый бар на шаге 2 (до этого цены нет),
// close 0.0200.
struct Fixture {
  Timeline timeline = Timeline::build(
      {kA, kB},
      {{bar(0, "100"), bar(1, "100"), bar(2, "100")}, {bar(2, "0.0200")}}, {});
  ExecutionEngine engine{{kA, kB}, CostConfig::defaults(), false};
  Portfolio portfolio{D("1000000"), 2};

  Validation check(const OrderSpec &spec, std::int64_t step = 2) const {
    return engine.validate(spec, portfolio, timeline, step);
  }
  void hold(std::int64_t lots) {
    portfolio.applyFill(0, Side::Buy, lots, D("100"), D("5000.00"), D("2.50"),
                        D("0.50"), 0, 1);
  }
};

OrderSpec spec(Side side, OrderType type, std::int64_t lots,
               int instrument = 0) {
  OrderSpec s;
  s.instrument = instrument;
  s.side = side;
  s.type = type;
  s.lots = lots;
  return s;
}

void expectInvalid(const Validation &v, const char *field) {
  EXPECT_FALSE(v.ok);
  EXPECT_EQ(v.reason, Reason::InvalidParams);
  EXPECT_EQ(v.field, field);
}

TEST(Validation, ValidOrdersPass) {
  Fixture f;
  EXPECT_TRUE(f.check(spec(Side::Buy, OrderType::Market, 10)).ok);
  OrderSpec limit = spec(Side::Buy, OrderType::Limit, 10);
  limit.limitPrice = D("99");
  EXPECT_TRUE(f.check(limit).ok);
  OrderSpec stop = spec(Side::Buy, OrderType::Stop, 10);
  stop.stopPrice = D("101");
  EXPECT_TRUE(f.check(stop).ok);
  OrderSpec stopLimit = spec(Side::Buy, OrderType::StopLimit, 10);
  stopLimit.stopPrice = D("101");
  stopLimit.limitPrice = D("102");
  EXPECT_TRUE(f.check(stopLimit).ok);
}

TEST(Validation, StopWrongSide) {
  Fixture f;
  // Стоп на продажу должен быть ниже последнего close (100).
  OrderSpec sellStop = spec(Side::Sell, OrderType::Stop, 1);
  sellStop.stopPrice = D("101");
  expectInvalid(f.check(sellStop), "stopPrice");
  // Стоп на покупку - выше.
  OrderSpec buyStop = spec(Side::Buy, OrderType::Stop, 1);
  buyStop.stopPrice = D("99");
  expectInvalid(f.check(buyStop), "stopPrice");
}

TEST(Validation, StopAtCloseIsWrongSide) {
  Fixture f;
  OrderSpec buyStop = spec(Side::Buy, OrderType::Stop, 1);
  buyStop.stopPrice = D("100");
  expectInvalid(f.check(buyStop), "stopPrice");
}

TEST(Validation, SellStopBelowCloseWithPosition) {
  Fixture f;
  f.hold(5);
  OrderSpec sellStop = spec(Side::Sell, OrderType::Stop, 5);
  sellStop.stopPrice = D("95");
  EXPECT_TRUE(f.check(sellStop).ok);
}

TEST(Validation, LotsMustBePositive) {
  Fixture f;
  expectInvalid(f.check(spec(Side::Buy, OrderType::Market, 0)), "lots");
  expectInvalid(f.check(spec(Side::Buy, OrderType::Market, -3)), "lots");
}

TEST(Validation, PricesMustBePositiveAndPresent) {
  Fixture f;
  OrderSpec limit = spec(Side::Buy, OrderType::Limit, 1);
  expectInvalid(f.check(limit), "limitPrice");
  limit.limitPrice = D("0");
  expectInvalid(f.check(limit), "limitPrice");
  limit.limitPrice = D("-1");
  expectInvalid(f.check(limit), "limitPrice");

  OrderSpec stop = spec(Side::Buy, OrderType::Stop, 1);
  expectInvalid(f.check(stop), "stopPrice");
  stop.stopPrice = D("0");
  expectInvalid(f.check(stop), "stopPrice");

  OrderSpec stopLimit = spec(Side::Buy, OrderType::StopLimit, 1);
  stopLimit.stopPrice = D("101");
  expectInvalid(f.check(stopLimit), "limitPrice");
}

TEST(Validation, TrailingPercentRange) {
  Fixture f;
  f.hold(5);
  OrderSpec trailing = spec(Side::Sell, OrderType::TrailingStop, 5);
  expectInvalid(f.check(trailing), "trailingPercent");
  trailing.trailingPercent = D("0");
  expectInvalid(f.check(trailing), "trailingPercent");
  trailing.trailingPercent = D("50.000000001");
  expectInvalid(f.check(trailing), "trailingPercent");
  trailing.trailingPercent = D("50");
  EXPECT_TRUE(f.check(trailing).ok);
  trailing.trailingPercent = D("0.000000001");
  EXPECT_TRUE(f.check(trailing).ok);
}

TEST(Validation, TakeProfitAndTrailingAreSellOnly) {
  Fixture f;
  OrderSpec take = spec(Side::Buy, OrderType::TakeProfit, 1);
  take.limitPrice = D("110");
  expectInvalid(f.check(take), "side");
  OrderSpec trailing = spec(Side::Buy, OrderType::TrailingStop, 1);
  trailing.trailingPercent = D("5");
  expectInvalid(f.check(trailing), "side");
}

TEST(Validation, TakeProfitMustBeAboveClose) {
  Fixture f;
  f.hold(5);
  OrderSpec take = spec(Side::Sell, OrderType::TakeProfit, 5);
  take.limitPrice = D("99");
  expectInvalid(f.check(take), "limitPrice");
  take.limitPrice = D("110");
  EXPECT_TRUE(f.check(take).ok);
}

TEST(Validation, InsufficientCashUsesLastCloseWithCommissionAndFee) {
  // 100 лотов * 10 * 100 = 100 000; с комиссией и сбором 100 000 * 1.0006 = 100
  // 060.
  Fixture f;
  const OrderSpec order = spec(Side::Buy, OrderType::Market, 100);
  Portfolio enough(D("100060"), 2);
  EXPECT_TRUE(f.engine.validate(order, enough, f.timeline, 2).ok);
  Portfolio notEnough(D("100059.99"), 2);
  const Validation v = f.engine.validate(order, notEnough, f.timeline, 2);
  EXPECT_FALSE(v.ok);
  EXPECT_EQ(v.reason, Reason::InsufficientCash);
}

TEST(Validation, CashCheckAlsoForLimitOrdersAtLastClose) {
  Fixture f;
  OrderSpec limit =
      spec(Side::Buy, OrderType::Limit, 100000); // 100 000 000 ₽ при close 100
  limit.limitPrice = D("1");
  const Validation v = f.check(limit);
  EXPECT_FALSE(v.ok);
  EXPECT_EQ(v.reason, Reason::InsufficientCash);
}

TEST(Validation, HugeOrderIsInsufficientCashNotOverflow) {
  Fixture f;
  const Validation v = f.check(spec(Side::Buy, OrderType::Market,
                                    std::numeric_limits<std::int64_t>::max()));
  EXPECT_FALSE(v.ok);
  EXPECT_EQ(v.reason, Reason::InsufficientCash);
}

TEST(Validation, InsufficientPosition) {
  Fixture f;
  f.hold(5);
  EXPECT_TRUE(f.check(spec(Side::Sell, OrderType::Market, 5)).ok);
  const Validation v = f.check(spec(Side::Sell, OrderType::Market, 6));
  EXPECT_FALSE(v.ok);
  EXPECT_EQ(v.reason, Reason::InsufficientPosition);
  EXPECT_TRUE(v.field.empty());
}

TEST(Validation, UnknownOrUntradedInstrument) {
  Fixture f;
  expectInvalid(f.check(spec(Side::Buy, OrderType::Market, 1, 2)),
                "instrument");
  expectInvalid(f.check(spec(Side::Buy, OrderType::Market, 1, -1)),
                "instrument");
  // У B на шаге 1 ещё не было баров - цены для проверки нет.
  expectInvalid(f.check(spec(Side::Buy, OrderType::Market, 1, 1), 1),
                "instrument");
  EXPECT_TRUE(f.check(spec(Side::Buy, OrderType::Market, 1, 1), 2).ok);
}

TEST(Validation, UsesLastCloseOfOwnInstrument) {
  // Цена B - 0.0200 (бар шага 2): 1 лот = 10 000 штук = 200 ₽, с издержками
  // 200.12.
  Fixture f;
  const OrderSpec order = spec(Side::Buy, OrderType::Market, 1, 1);
  Portfolio poor(D("200.11"), 2);
  EXPECT_EQ(f.engine.validate(order, poor, f.timeline, 2).reason,
            Reason::InsufficientCash);
  Portfolio enough(D("200.12"), 2);
  EXPECT_TRUE(f.engine.validate(order, enough, f.timeline, 2).ok);
}

} // namespace
