#include "core/portfolio/portfolio.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace core {

namespace {

constexpr int kKopeckScale = 2;

// value * part / whole до копейки HALF_UP одним шагом.
Decimal share(const Decimal &Value, std::int64_t Part, std::int64_t Whole) {
  return Value.mulDiv(Decimal::fromUnits(Part), Decimal::fromUnits(Whole),
                      kKopeckScale, RoundingMode::HalfUp);
}

} // namespace

Portfolio::Portfolio(Decimal InitialCash, std::size_t InstrumentCount)
    : Cash(InitialCash), Positions(InstrumentCount) {}

Portfolio::Position &Portfolio::position(int Instrument) {
  if (Instrument < 0 ||
      static_cast<std::size_t>(Instrument) >= Positions.size()) {
    throw std::out_of_range("Portfolio: нет бумаги с номером " +
                            std::to_string(Instrument));
  }
  return Positions[static_cast<std::size_t>(Instrument)];
}

const Portfolio::Position &Portfolio::position(int Instrument) const {
  if (Instrument < 0 ||
      static_cast<std::size_t>(Instrument) >= Positions.size()) {
    throw std::out_of_range("Portfolio: нет бумаги с номером " +
                            std::to_string(Instrument));
  }
  return Positions[static_cast<std::size_t>(Instrument)];
}

std::int64_t Portfolio::lots(int Instrument) const {
  return position(Instrument).Lots;
}

Decimal Portfolio::avgPrice(int Instrument) const {
  return position(Instrument).AvgPrice;
}

void Portfolio::applyFill(int Instrument, Side Side, std::int64_t Lots,
                          const Decimal &Price, const Decimal &Value,
                          const Decimal &Commission, const Decimal &Fee,
                          std::int64_t Step, std::int64_t OrderId) {
  Position &Pos = position(Instrument);
  if (Lots <= 0)
    throw std::invalid_argument(
        "Portfolio: число лотов должно быть положительным");
  if (Price.sign() < 0 || Value.sign() < 0 || Commission.sign() < 0 ||
      Fee.sign() < 0) {
    throw std::invalid_argument(
        "Portfolio: цена, сумма, комиссия и сбор не могут быть отрицательными");
  }

  if (Side == Side::Buy) {
    // (старые лоты * старая средняя + новые лоты * цена) / все лоты, один шаг
    // округления.
    const Decimal Total = Pos.AvgPrice.times(Pos.Lots) + Price.times(Lots);
    const Decimal NewCash = Cash - (Value + Commission + Fee);
    const Decimal NewAvg = Total.div(Decimal::fromUnits(Pos.Lots + Lots),
                                     Decimal::kScale, RoundingMode::HalfUp);
    // Всё, что может выбросить исключение, посчитано выше: состояние меняется
    // целиком или никак.
    Cash = NewCash;
    Pos.Lots += Lots;
    Pos.AvgPrice = NewAvg;
    Pos.Open.push_back(
        OpenLot{Step, OrderId, Lots, Lots, Price, Value, Commission + Fee});
    return;
  }

  if (Lots > Pos.Lots) {
    throw std::logic_error("Portfolio: продажа " + std::to_string(Lots) +
                           " лотов больше позиции " + std::to_string(Pos.Lots));
  }
  const Decimal NewCash = Cash + (Value - Commission - Fee);
  const Decimal SellCosts = Commission + Fee;
  std::vector<RoundTrip> Trips;
  std::int64_t ToMatch = Lots;
  for (OpenLot &Buy : Pos.Open) {
    if (ToMatch == 0)
      break;
    const std::int64_t Matched = std::min(ToMatch, Buy.LotsLeft);
    RoundTrip Trip;
    Trip.Instrument = Instrument;
    Trip.EntryStep = Buy.Step;
    Trip.ExitStep = Step;
    Trip.EntryOrderId = Buy.OrderId;
    Trip.Lots = Matched;
    Trip.EntryPrice = Buy.Price;
    Trip.ExitPrice = Price;
    Trip.EntryValue = share(Buy.Value, Matched, Buy.Lots);
    Trip.ExitValue = share(Value, Matched, Lots);
    Trip.Costs =
        share(Buy.Costs, Matched, Buy.Lots) + share(SellCosts, Matched, Lots);
    Trip.NetPnl = Trip.ExitValue - Trip.EntryValue - Trip.Costs;
    Trips.push_back(Trip);
    ToMatch -= Matched;
  }
  // Применяем после расчёта: при переполнении Decimal состояние не портится.
  ToMatch = Lots;
  while (ToMatch > 0) {
    OpenLot &Buy = Pos.Open.front();
    const std::int64_t Matched = std::min(ToMatch, Buy.LotsLeft);
    Buy.LotsLeft -= Matched;
    ToMatch -= Matched;
    if (Buy.LotsLeft == 0)
      Pos.Open.pop_front();
  }
  Cash = NewCash;
  RoundTrips.insert(RoundTrips.end(), Trips.begin(), Trips.end());
  Pos.Lots -= Lots;
  if (Pos.Lots == 0)
    Pos.AvgPrice = Decimal::zero();
}

Decimal Portfolio::costBasis(int Instrument) const {
  Decimal Sum;
  for (const OpenLot &Lot : position(Instrument).Open)
    Sum += share(Lot.Value, Lot.LotsLeft, Lot.Lots);
  return Sum;
}

Decimal Portfolio::positionValue(std::int64_t Lots, std::int64_t LotSize,
                                 const Decimal &Close) {
  return Close.times(Lots).times(LotSize).round(kKopeckScale,
                                                RoundingMode::HalfUp);
}

Decimal Portfolio::equity(const std::vector<std::optional<Decimal>> &LastClose,
                          const std::vector<std::int64_t> &LotSizes) const {
  if (LastClose.size() != Positions.size() ||
      LotSizes.size() != Positions.size()) {
    throw std::invalid_argument("Portfolio::equity: размеры lastClose и "
                                "lotSizes должны равняться числу бумаг");
  }
  Decimal Total = Cash;
  for (std::size_t I = 0; I < Positions.size(); ++I) {
    if (Positions[I].Lots == 0)
      continue;
    if (!LastClose[I]) {
      throw std::logic_error("Portfolio::equity: у бумаги " +
                             std::to_string(I) +
                             " есть позиция, но нет цены close");
    }
    Total += positionValue(Positions[I].Lots, LotSizes[I], *LastClose[I]);
  }
  return Total;
}

} // namespace core
