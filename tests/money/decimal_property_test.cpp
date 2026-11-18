// Тесты на свойства (property-based) для инварианта INV-6 (деньги, Decimal,
// портфель).
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <vector>

#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp"

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

RC_GTEST_PROP(DecimalProperty, PortfolioCashConservation, ()) {
  const auto InitialRubles =
      *rc::gen::inRange<std::int32_t>(100'000, 5'000'000);

  const Decimal InitialCash = Decimal::fromUnits(InitialRubles);
  Portfolio Port(InitialCash, 1);

  Decimal TotalBought;
  Decimal TotalSold;
  Decimal TotalCosts;

  const auto TradeCount = *rc::gen::inRange<int>(1, 15);
  for (int I = 0; I < TradeCount; ++I) {
    const auto LotsToBuy = *rc::gen::inRange<std::int64_t>(1, 10);
    const auto PriceUnits = *rc::gen::inRange<std::int64_t>(100, 500);
    const Decimal Price = Decimal::fromUnits(PriceUnits);
    const Decimal Value = Price.times(LotsToBuy);
    const Decimal Commission =
        Value.mul(Decimal::parse("0.003"), 2, RoundingMode::CEILING);
    const Decimal Fee =
        Value.mul(Decimal::parse("0.0001"), 2, RoundingMode::CEILING);

    if (Port.cash() < Value + Commission + Fee) {
      break;
    }

    Port.applyFill(0, Side::Buy, LotsToBuy, Price, Value, Commission, Fee, I,
                   I + 1);
    TotalBought = TotalBought + Value;
    TotalCosts = TotalCosts + Commission + Fee;

    const auto CurrentLots = Port.lots(0);
    if (CurrentLots > 0 && *rc::gen::arbitrary<bool>()) {
      const auto LotsToSell =
          *rc::gen::inRange<std::int64_t>(1, CurrentLots + 1);
      const Decimal SellValue = Price.times(LotsToSell);
      const Decimal SellCommission =
          SellValue.mul(Decimal::parse("0.003"), 2, RoundingMode::CEILING);
      const Decimal SellFee =
          SellValue.mul(Decimal::parse("0.0001"), 2, RoundingMode::CEILING);

      Port.applyFill(0, Side::Sell, LotsToSell, Price, SellValue,
                     SellCommission, SellFee, I, I + 100);
      TotalSold = TotalSold + SellValue;
      TotalCosts = TotalCosts + SellCommission + SellFee;
    }
  }

  // Инвариант INV-6: cash = initial - bought + sold - costs до копейки
  const Decimal ExpectedCash =
      InitialCash - TotalBought + TotalSold - TotalCosts;
  RC_ASSERT(Port.cash() == ExpectedCash);
  RC_ASSERT(Port.cash() >= Decimal::zero());
  RC_ASSERT(Port.lots(0) >= 0);
}

} // namespace
} // namespace core
