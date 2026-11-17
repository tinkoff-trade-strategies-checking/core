#include "core/portfolio/portfolio.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace core {

namespace {

constexpr int kKopeckScale = 2;

// value * part / whole до копейки HALF_UP одним шагом.
Decimal share(const Decimal &value, std::int64_t part, std::int64_t whole) {
  return value.mulDiv(Decimal::fromUnits(part), Decimal::fromUnits(whole),
                      kKopeckScale, RoundingMode::HALF_UP);
}

} // namespace

Portfolio::Portfolio(Decimal initialCash, std::size_t instrumentCount)
    : cash_(initialCash), positions_(instrumentCount) {}

Portfolio::Position &Portfolio::position(int instrument) {
  if (instrument < 0 ||
      static_cast<std::size_t>(instrument) >= positions_.size()) {
    throw std::out_of_range("Portfolio: нет бумаги с номером " +
                            std::to_string(instrument));
  }
  return positions_[static_cast<std::size_t>(instrument)];
}

const Portfolio::Position &Portfolio::position(int instrument) const {
  if (instrument < 0 ||
      static_cast<std::size_t>(instrument) >= positions_.size()) {
    throw std::out_of_range("Portfolio: нет бумаги с номером " +
                            std::to_string(instrument));
  }
  return positions_[static_cast<std::size_t>(instrument)];
}

std::int64_t Portfolio::lots(int instrument) const {
  return position(instrument).lots;
}

Decimal Portfolio::avgPrice(int instrument) const {
  return position(instrument).avgPrice;
}

void Portfolio::applyFill(int instrument, Side side, std::int64_t lots,
                          const Decimal &price, const Decimal &value,
                          const Decimal &commission, const Decimal &fee,
                          std::int64_t step, std::int64_t orderId) {
  Position &pos = position(instrument);
  if (lots <= 0)
    throw std::invalid_argument(
        "Portfolio: число лотов должно быть положительным");
  if (price.sign() < 0 || value.sign() < 0 || commission.sign() < 0 ||
      fee.sign() < 0) {
    throw std::invalid_argument(
        "Portfolio: цена, сумма, комиссия и сбор не могут быть отрицательными");
  }

  if (side == Side::Buy) {
    // (старые лоты * старая средняя + новые лоты * цена) / все лоты, один шаг
    // округления.
    const Decimal total = pos.avgPrice.times(pos.lots) + price.times(lots);
    const Decimal newCash = cash_ - (value + commission + fee);
    const Decimal newAvg = total.div(Decimal::fromUnits(pos.lots + lots),
                                     Decimal::kScale, RoundingMode::HALF_UP);
    // Всё, что может выбросить исключение, посчитано выше: состояние меняется
    // целиком или никак.
    cash_ = newCash;
    pos.lots += lots;
    pos.avgPrice = newAvg;
    pos.open.push_back(
        OpenLot{step, orderId, lots, lots, value, commission + fee});
    return;
  }

  if (lots > pos.lots) {
    throw std::logic_error("Portfolio: продажа " + std::to_string(lots) +
                           " лотов больше позиции " + std::to_string(pos.lots));
  }
  const Decimal newCash = cash_ + (value - commission - fee);
  const Decimal sellCosts = commission + fee;
  std::vector<RoundTrip> trips;
  std::int64_t toMatch = lots;
  for (OpenLot &buy : pos.open) {
    if (toMatch == 0)
      break;
    const std::int64_t matched = std::min(toMatch, buy.lotsLeft);
    RoundTrip trip;
    trip.instrument = instrument;
    trip.entryStep = buy.step;
    trip.exitStep = step;
    trip.entryOrderId = buy.orderId;
    trip.lots = matched;
    trip.entryValue = share(buy.value, matched, buy.lots);
    trip.exitValue = share(value, matched, lots);
    trip.costs =
        share(buy.costs, matched, buy.lots) + share(sellCosts, matched, lots);
    trip.netPnl = trip.exitValue - trip.entryValue - trip.costs;
    trips.push_back(trip);
    toMatch -= matched;
  }
  // Применяем после расчёта: при переполнении Decimal состояние не портится.
  toMatch = lots;
  while (toMatch > 0) {
    OpenLot &buy = pos.open.front();
    const std::int64_t matched = std::min(toMatch, buy.lotsLeft);
    buy.lotsLeft -= matched;
    toMatch -= matched;
    if (buy.lotsLeft == 0)
      pos.open.pop_front();
  }
  cash_ = newCash;
  roundTrips_.insert(roundTrips_.end(), trips.begin(), trips.end());
  pos.lots -= lots;
  if (pos.lots == 0)
    pos.avgPrice = Decimal::zero();
}

Decimal Portfolio::positionValue(std::int64_t lots, std::int64_t lotSize,
                                 const Decimal &close) {
  return close.times(lots).times(lotSize).round(kKopeckScale,
                                                RoundingMode::HALF_UP);
}

Decimal Portfolio::equity(const std::vector<std::optional<Decimal>> &lastClose,
                          const std::vector<std::int64_t> &lotSizes) const {
  if (lastClose.size() != positions_.size() ||
      lotSizes.size() != positions_.size()) {
    throw std::invalid_argument("Portfolio::equity: размеры lastClose и "
                                "lotSizes должны равняться числу бумаг");
  }
  Decimal total = cash_;
  for (std::size_t i = 0; i < positions_.size(); ++i) {
    if (positions_[i].lots == 0)
      continue;
    if (!lastClose[i]) {
      throw std::logic_error("Portfolio::equity: у бумаги " +
                             std::to_string(i) +
                             " есть позиция, но нет цены close");
    }
    total += positionValue(positions_[i].lots, lotSizes[i], *lastClose[i]);
  }
  return total;
}

} // namespace core
