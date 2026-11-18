// Тесты на свойства (property-based) для инварианта INV-6 (деньги, Decimal,
// портфель).
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <vector>

#include "core/money/decimal.hpp"

namespace rc {

template <> struct Arbitrary<core::Decimal> {
  static Gen<core::Decimal> arbitrary() {
    return gen::exec([] {
      const auto Units = *gen::inRange<std::int64_t>(-100'000'000, 100'000'000);
      auto Nano = *gen::inRange<std::int32_t>(0, 1'000'000'000);
      if (Units < 0) {
        Nano = -Nano;
      } else if (Units == 0 && *gen::arbitrary<bool>()) {
        Nano = -Nano;
      }
      return core::Decimal::fromUnitsNano(Units, Nano);
    });
  }
};

} // namespace rc

namespace core {
namespace {

rc::Gen<Decimal> genPositiveStep() {
  return rc::gen::exec([] {
    const auto Units = *rc::gen::inRange<std::int64_t>(0, 10);
    auto Nano = *rc::gen::inRange<std::int32_t>(100'000, 1'000'000'000);
    if (Units == 0 && Nano == 0) {
      Nano = 10'000'000;
    }
    return Decimal::fromUnitsNano(Units, Nano);
  });
}

RC_GTEST_PROP(DecimalProperty, AdditionCommutativeAndAssociative,
              (const Decimal &A, const Decimal &B, const Decimal &C)) {
  RC_ASSERT(A + B == B + A);
  RC_ASSERT((A + B) + C == A + (B + C));
}

RC_GTEST_PROP(DecimalProperty, SubtractionInversionAndIdentity,
              (const Decimal &A, const Decimal &B)) {
  RC_ASSERT((A + B) - B == A);
  RC_ASSERT(A - A == Decimal::zero());
  RC_ASSERT(A - Decimal::zero() == A);
}

RC_GTEST_PROP(DecimalProperty, MultiplyByIntegerLinear, (const Decimal &A)) {
  const auto N = *rc::gen::inRange<std::int32_t>(-1000, 1000);
  RC_ASSERT(A.times(N) + A == A.times(N + 1));
}

RC_GTEST_PROP(DecimalProperty, DirectedRoundingFloorAndCeiling,
              (const Decimal &D)) {
  const Decimal Step = *genPositiveStep();
  RC_PRE(!Step.isZero());

  const Decimal Floored = D.roundToStep(Step, RoundingMode::FLOOR);
  const Decimal Ceiled = D.roundToStep(Step, RoundingMode::CEILING);

  RC_ASSERT(Floored <= D);
  RC_ASSERT(Ceiled >= D);
  RC_ASSERT(Floored.roundToStep(Step, RoundingMode::FLOOR) == Floored);
  RC_ASSERT(Ceiled.roundToStep(Step, RoundingMode::CEILING) == Ceiled);
  RC_ASSERT(Ceiled - Floored <= Step);
}

} // namespace
} // namespace core
