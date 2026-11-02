#include "core/exec/costs.hpp"

#include <stdexcept>

namespace core {

CostConfig CostConfig::defaults() {
  CostConfig c;
  c.commissionRate = Decimal::parse("0.0005");
  c.exchangeFeeRate = Decimal::parse("0.0001");
  c.spread = Decimal::parse("0.001");
  c.slippageKappa = Decimal::parse("0.1");
  c.liquidityShare = Decimal::parse("0.10");
  return c;
}

CostConfig CostConfig::zero() { return CostConfig{}; }

namespace {

Decimal halfSpread(const CostConfig& costs) {
  return costs.spread.div(Decimal::fromUnits(2), Decimal::kScale, RoundingMode::HALF_UP);
}

}  // namespace

Decimal fillPrice(Side side, const Decimal& base, std::int64_t lots, std::int64_t barVolumeLots, const CostConfig& costs,
                  const Decimal& priceStep) {
  if (lots <= 0) throw std::invalid_argument("fillPrice: число лотов должно быть положительным");
  const Decimal frac = (costs.slippageKappa.isZero() || barVolumeLots <= 0)
                           ? Decimal::zero()
                           : costs.slippageKappa.times(lots).div(Decimal::fromUnits(barVolumeLots), Decimal::kScale,
                                                                 RoundingMode::HALF_UP);
  const Decimal half = halfSpread(costs);
  const bool buy = side == Side::Buy;
  const Decimal m = buy ? Decimal::one() + half + frac : Decimal::one() - half - frac;
  // Покупка вверх, продажа вниз - оба шага округления одним режимом (`decisions.md`, п. 1).
  const RoundingMode mode = buy ? RoundingMode::CEILING : RoundingMode::FLOOR;
  const Decimal price = base.mul(m, Decimal::kScale, mode).roundToStep(priceStep, mode);
  // При q >> V множитель продажи может стать отрицательным: цена не ниже одного шага.
  return (!buy && price < priceStep) ? priceStep : price;
}

Amounts amounts(const Decimal& price, std::int64_t lots, std::int64_t lotSize, const CostConfig& costs) {
  Amounts a;
  a.value = price.times(lots).times(lotSize).round(2, RoundingMode::HALF_UP);
  a.commission = a.value.mul(costs.commissionRate, 2, RoundingMode::CEILING);
  a.fee = a.value.mul(costs.exchangeFeeRate, 2, RoundingMode::CEILING);
  return a;
}

CostSplit costSplit(const Decimal& base, const Decimal& price, std::int64_t lots, std::int64_t lotSize,
                    const CostConfig& costs) {
  CostSplit s;
  // base·lots·lotSize·s/2 одним шагом округления.
  s.spreadCost = base.times(lots).times(lotSize).mulDiv(costs.spread, Decimal::fromUnits(2), 2, RoundingMode::HALF_UP);
  const Decimal total = (price - base).abs().times(lots).times(lotSize).round(2, RoundingMode::HALF_UP);
  s.slippageCost = total - s.spreadCost;
  return s;
}

}  // namespace core
