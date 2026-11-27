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

Portfolio::Portfolio(Decimal InitialCash, std::size_t InstrumentCount,
                     bool ShortAllowed)
    : Cash(InitialCash), AllowShort(ShortAllowed), Positions(InstrumentCount) {}

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

void Portfolio::chargeFee(const Decimal &Amount) {
  if (Amount.sign() < 0)
    throw std::invalid_argument("Portfolio: списание не может быть "
                                "отрицательным");
  if (Amount.isZero())
    return;
  Cash -= Amount;
}

void Portfolio::applyFill(int Instrument, Side OrderSide, std::int64_t Lots,
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

  if (OrderSide == Side::Sell && !AllowShort && Lots > Pos.Lots) {
    throw std::logic_error("Portfolio: продажа " + std::to_string(Lots) +
                           " лотов больше позиции " + std::to_string(Pos.Lots));
  }

  // Переворот позиции (лонг -> шорт или шорт -> лонг): разбиваем на закрытие
  // текущей позиции и открытие остатка в противоположную сторону с точным
  // распределением суммы и издержек до копейки.
  if ((OrderSide == Side::Sell && Pos.Lots > 0 && Lots > Pos.Lots) ||
      (OrderSide == Side::Buy && Pos.Lots < 0 && Lots > -Pos.Lots)) {
    const std::int64_t CloseLots = Pos.Lots > 0 ? Pos.Lots : -Pos.Lots;
    const std::int64_t OpenLots = Lots - CloseLots;
    const Decimal CloseValue = share(Value, CloseLots, Lots);
    const Decimal CloseComm = share(Commission, CloseLots, Lots);
    const Decimal CloseFee = share(Fee, CloseLots, Lots);
    applyFill(Instrument, OrderSide, CloseLots, Price, CloseValue, CloseComm,
              CloseFee, Step, OrderId);
    applyFill(Instrument, OrderSide, OpenLots, Price, Value - CloseValue,
              Commission - CloseComm, Fee - CloseFee, Step, OrderId);
    return;
  }

  const bool OpensOrAdds = (OrderSide == Side::Buy && Pos.Lots >= 0) ||
                           (OrderSide == Side::Sell && Pos.Lots <= 0);
  if (OpensOrAdds) {
    const std::int64_t AbsOld = Pos.Lots >= 0 ? Pos.Lots : -Pos.Lots;
    const std::int64_t AbsNew = AbsOld + Lots;
    const Decimal Total = Pos.AvgPrice.times(AbsOld) + Price.times(Lots);
    const Decimal NewCash = (OrderSide == Side::Buy)
                                ? (Cash - (Value + Commission + Fee))
                                : (Cash + (Value - Commission - Fee));
    const Decimal NewAvg = Total.div(Decimal::fromUnits(AbsNew),
                                     Decimal::kScale, RoundingMode::HalfUp);
    Cash = NewCash;
    Pos.Lots = (OrderSide == Side::Buy) ? AbsNew : -AbsNew;
    Pos.AvgPrice = NewAvg;
    Pos.Open.push_back(
        OpenLot{Step, OrderId, Lots, Lots, Price, Value, Commission + Fee});
    return;
  }

  // Закрытие (полное или частичное) существующей позиции по FIFO:
  // OrderSide == Sell закрывает лонг (Pos.Lots > 0),
  // OrderSide == Buy закрывает шорт (Pos.Lots < 0).
  const bool ClosingShort = (OrderSide == Side::Buy);
  const Decimal NewCash = ClosingShort ? (Cash - (Value + Commission + Fee))
                                       : (Cash + (Value - Commission - Fee));
  const Decimal ExitCosts = Commission + Fee;
  std::vector<RoundTrip> Trips;
  std::int64_t ToMatch = Lots;
  for (OpenLot &Entry : Pos.Open) {
    if (ToMatch == 0)
      break;
    const std::int64_t Matched = std::min(ToMatch, Entry.LotsLeft);
    RoundTrip Trip;
    Trip.Instrument = Instrument;
    Trip.EntryStep = Entry.Step;
    Trip.ExitStep = Step;
    Trip.EntryOrderId = Entry.OrderId;
    Trip.Lots = Matched;
    Trip.EntryPrice = Entry.Price;
    Trip.ExitPrice = Price;
    Trip.EntryValue = share(Entry.Value, Matched, Entry.Lots);
    Trip.ExitValue = share(Value, Matched, Lots);
    Trip.Costs = share(Entry.Costs, Matched, Entry.Lots) +
                 share(ExitCosts, Matched, Lots);
    Trip.IsShort = ClosingShort;
    Trip.NetPnl = Trip.grossPnl() - Trip.Costs;
    Trips.push_back(Trip);
    ToMatch -= Matched;
  }
  // Применяем после расчёта: при переполнении Decimal состояние не портится.
  ToMatch = Lots;
  while (ToMatch > 0) {
    OpenLot &Entry = Pos.Open.front();
    const std::int64_t Matched = std::min(ToMatch, Entry.LotsLeft);
    Entry.LotsLeft -= Matched;
    ToMatch -= Matched;
    if (Entry.LotsLeft == 0)
      Pos.Open.pop_front();
  }
  Cash = NewCash;
  RoundTrips.insert(RoundTrips.end(), Trips.begin(), Trips.end());
  Pos.Lots += ClosingShort ? Lots : -Lots;
  if (Pos.Lots == 0)
    Pos.AvgPrice = Decimal::zero();
}

Decimal Portfolio::costBasis(int Instrument) const {
  const Position &Pos = position(Instrument);
  Decimal Sum;
  for (const OpenLot &Lot : Pos.Open)
    Sum += share(Lot.Value, Lot.LotsLeft, Lot.Lots);
  return Pos.Lots < 0 ? -Sum : Sum;
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
