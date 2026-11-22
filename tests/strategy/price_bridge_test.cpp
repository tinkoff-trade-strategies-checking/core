#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

#include "core/strategy/price_bridge.hpp"

namespace core {
namespace {

TEST(PriceBridge, NormalPricesHalfUp) {
  const Decimal Step = Decimal::parse("0.01");
  EXPECT_EQ(roundDslPriceToStep(123.45, Step), Decimal::parse("123.45"));
  EXPECT_EQ(roundDslPriceToStep(0.005, Step), Decimal::parse("0.01"));
  EXPECT_EQ(roundDslPriceToStep(0.015, Step), Decimal::parse("0.02"));
  EXPECT_EQ(roundDslPriceToStep(10.024, Step), Decimal::parse("10.02"));
  EXPECT_EQ(roundDslPriceToStep(10.025, Step), Decimal::parse("10.03"));

  const Decimal Step05 = Decimal::parse("0.05");
  EXPECT_EQ(roundDslPriceToStep(0.025, Step05), Decimal::parse("0.05"));
  EXPECT_EQ(roundDslPriceToStep(0.0249, Step05), Decimal::zero());
  EXPECT_EQ(roundDslPriceToStep(100.075, Step05), Decimal::parse("100.10"));
}

TEST(PriceBridge, ScientificNotation) {
  const Decimal Step1 = Decimal::parse("1");
  EXPECT_EQ(roundDslPriceToStep(1e15, Step1),
            Decimal::parse("1000000000000000"));

  const Decimal NanoStep = Decimal::fromRaw(1); // 10^-9
  EXPECT_EQ(roundDslPriceToStep(4e-10, NanoStep), Decimal::zero());
  EXPECT_EQ(roundDslPriceToStep(5e-10, NanoStep), Decimal::fromRaw(1));
  EXPECT_EQ(roundDslPriceToStep(1.5e-9, NanoStep), Decimal::fromRaw(2));

  const Decimal MicroStep = Decimal::parse("0.000001");
  EXPECT_EQ(roundDslPriceToStep(1.25e-5, MicroStep),
            Decimal::parse("0.000013"));
}

TEST(PriceBridge, MoreThanNineDecimalsSingleStepHalfUp) {
  const Decimal Step = Decimal::parse("0.01");
  // Без двойного округления: 0.0049999999999 < 0.005 -> 0.00 (а не 0.01).
  EXPECT_EQ(roundDslPriceToStep(0.0049999999999, Step), Decimal::zero());
  EXPECT_EQ(roundDslPriceToStep(0.0050000000001, Step), Decimal::parse("0.01"));
  EXPECT_EQ(roundDslPriceToStep(10.004999999999, Step),
            Decimal::parse("10.00"));
  EXPECT_EQ(roundDslPriceToStep(10.005000000001, Step),
            Decimal::parse("10.01"));

  // Граница половины на 10-м знаке для шага 10^-9.
  const Decimal NanoStep = Decimal::fromRaw(1);
  EXPECT_EQ(roundDslPriceToStep(0.00000000049, NanoStep), Decimal::zero());
  EXPECT_EQ(roundDslPriceToStep(0.0000000005, NanoStep), Decimal::fromRaw(1));
  EXPECT_EQ(roundDslPriceToStep(1.0000000005, NanoStep),
            Decimal::fromRaw(1'000'000'001));
}

TEST(PriceBridge, SubStepZeroing) {
  EXPECT_EQ(roundDslPriceToStep(0.004, Decimal::parse("0.01")),
            Decimal::zero());
  EXPECT_EQ(roundDslPriceToStep(0.01, Decimal::parse("0.05")), Decimal::zero());
  EXPECT_EQ(roundDslPriceToStep(1e-25, Decimal::fromRaw(1)), Decimal::zero());
  EXPECT_EQ(roundDslPriceToStep(1e-300, Decimal::parse("0.01")),
            Decimal::zero());
}

TEST(PriceBridge, RejectsInvalidInputs) {
  const Decimal Step = Decimal::parse("0.01");
  EXPECT_THROW(roundDslPriceToStep(0.0, Step), std::invalid_argument);
  EXPECT_THROW(roundDslPriceToStep(-1.0, Step), std::invalid_argument);
  EXPECT_THROW(
      roundDslPriceToStep(std::numeric_limits<double>::quiet_NaN(), Step),
      std::invalid_argument);
  EXPECT_THROW(
      roundDslPriceToStep(std::numeric_limits<double>::infinity(), Step),
      std::invalid_argument);
  EXPECT_THROW(roundDslPriceToStep(1.0001e15, Step), std::invalid_argument);
  EXPECT_THROW(roundDslPriceToStep(10.0, Decimal::zero()),
               std::invalid_argument);
  EXPECT_THROW(roundDslPriceToStep(10.0, Decimal::parse("-0.01")),
               std::invalid_argument);
}

} // namespace
} // namespace core
