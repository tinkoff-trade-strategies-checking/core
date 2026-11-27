#include "core/exec/costs.hpp"

#include <stdexcept>

namespace core {

CostConfig CostConfig::defaults() {
  CostConfig C;
  C.CommissionRate = Decimal::parse("0.003");
  C.ExchangeFeeRate = Decimal::parse("0.0001");
  C.Spread = Decimal::parse("0.001");
  C.SlippageKappa = Decimal::parse("0.1");
  C.LiquidityShare = Decimal::parse("0.10");
  return C;
}

CostConfig CostConfig::zero() { return CostConfig{}; }

void validateCostConfig(const CostConfig &Costs) {
  if (Costs.CommissionRate.sign() < 0)
    throw std::invalid_argument(
        "CostConfig: комиссия не может быть отрицательной");
  if (Costs.ExchangeFeeRate.sign() < 0)
    throw std::invalid_argument(
        "CostConfig: биржевой сбор не может быть отрицательным");
  if (Costs.Spread.sign() < 0)
    throw std::invalid_argument(
        "CostConfig: спред не может быть отрицательным");
  if (Costs.SlippageKappa.sign() < 0)
    throw std::invalid_argument(
        "CostConfig: коэффициент проскальзывания не может быть отрицательным");
  if (Costs.LiquidityShare.has_value() &&
      (*Costs.LiquidityShare <= Decimal::zero() ||
       *Costs.LiquidityShare > Decimal::fromUnits(1))) {
    throw std::invalid_argument(
        "CostConfig: доля ликвидности должна быть в (0; 1]");
  }
}

Decimal MarginConfig::effectiveInitialMarginRatio() const {
  if (InitialMarginRatio.sign() > 0)
    return InitialMarginRatio;
  if (MaxLeverage <= Decimal::one())
    return Decimal::one();
  return Decimal::one().div(MaxLeverage, Decimal::kScale, RoundingMode::HalfUp);
}

bool MarginConfig::isMarginEnabled() const {
  return AllowShort || MaxLeverage > Decimal::one() ||
         InitialMarginRatio.sign() > 0 || MaintenanceMarginRatio.sign() > 0 ||
         ShortBorrowAnnualPct.sign() > 0 || MarginLoanAnnualPct.sign() > 0;
}

void validateMarginConfig(const MarginConfig &Margin) {
  if (Margin.MaxLeverage < Decimal::one())
    throw std::invalid_argument(
        "MarginConfig: кредитное плечо должно быть >= 1");
  if (Margin.ShortBorrowAnnualPct.sign() < 0)
    throw std::invalid_argument(
        "MarginConfig: ставка переноса шорта не может быть отрицательной");
  if (Margin.MarginLoanAnnualPct.sign() < 0)
    throw std::invalid_argument(
        "MarginConfig: ставка маржинального займа не может быть отрицательной");
  if (Margin.InitialMarginRatio.sign() < 0 ||
      Margin.InitialMarginRatio > Decimal::one()) {
    throw std::invalid_argument("MarginConfig: Dнач должна быть в [0; 1]");
  }
  if (Margin.MaintenanceMarginRatio.sign() < 0 ||
      Margin.MaintenanceMarginRatio > Decimal::one()) {
    throw std::invalid_argument("MarginConfig: Dmin должна быть в [0; 1]");
  }
  if (Margin.MaintenanceMarginRatio.sign() > 0 &&
      Margin.MaintenanceMarginRatio > Margin.effectiveInitialMarginRatio()) {
    throw std::invalid_argument("MarginConfig: Dmin не может превышать Dнач");
  }
}

void validateExecutionRules(const ExecutionRules &Rules) {
  if (Rules.PriceBandPct.sign() < 0 ||
      Rules.PriceBandPct >= Decimal::fromUnits(100)) {
    throw std::invalid_argument(
        "ExecutionRules: ценовой коридор должен быть в [0; 100)");
  }
}

namespace {

Decimal halfSpread(const CostConfig &Costs) {
  return Costs.Spread.div(Decimal::fromUnits(2), Decimal::kScale,
                          RoundingMode::HalfUp);
}

} // namespace

Decimal fillPrice(Side Side, const Decimal &Base, std::int64_t Lots,
                  std::int64_t BarVolumeLots, const CostConfig &Costs,
                  const Decimal &PriceStep) {
  if (Lots <= 0)
    throw std::invalid_argument(
        "fillPrice: число лотов должно быть положительным");
  const Decimal Frac = (Costs.SlippageKappa.isZero() || BarVolumeLots <= 0)
                           ? Decimal::zero()
                           : Costs.SlippageKappa.times(Lots).div(
                                 Decimal::fromUnits(BarVolumeLots),
                                 Decimal::kScale, RoundingMode::HalfUp);
  const Decimal Half = halfSpread(Costs);
  const bool Buy = Side == Side::Buy;
  const Decimal M =
      Buy ? Decimal::one() + Half + Frac : Decimal::one() - Half - Frac;
  // Покупка вверх, продажа вниз - оба шага округления одним режимом
  // (`decisions.md`, п. 1).
  const RoundingMode Mode = Buy ? RoundingMode::CEILING : RoundingMode::FLOOR;
  const Decimal Price =
      Base.mul(M, Decimal::kScale, Mode).roundToStep(PriceStep, Mode);
  // При q >> V множитель продажи может стать отрицательным: цена не ниже одного
  // шага.
  return (!Buy && Price < PriceStep) ? PriceStep : Price;
}

Amounts amounts(const Decimal &Price, std::int64_t Lots, std::int64_t LotSize,
                const CostConfig &Costs) {
  Amounts A;
  A.Value = Price.times(Lots).times(LotSize).round(2, RoundingMode::HalfUp);
  A.Commission = A.Value.mul(Costs.CommissionRate, 2, RoundingMode::CEILING);
  A.Fee = A.Value.mul(Costs.ExchangeFeeRate, 2, RoundingMode::CEILING);
  return A;
}

CostSplit costSplit(const Decimal &Base, const Decimal &Price,
                    std::int64_t Lots, std::int64_t LotSize,
                    const CostConfig &Costs) {
  CostSplit S;
  // base·lots·lotSize·s/2 одним шагом округления.
  S.SpreadCost = Base.times(Lots).times(LotSize).mulDiv(
      Costs.Spread, Decimal::fromUnits(2), 2, RoundingMode::HalfUp);
  const Decimal Total = (Price - Base)
                            .abs()
                            .times(Lots)
                            .times(LotSize)
                            .round(2, RoundingMode::HalfUp);
  S.SlippageCost = Total - S.SpreadCost;
  return S;
}

} // namespace core
