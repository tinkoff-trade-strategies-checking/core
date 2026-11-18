#include "core/blind/blind.hpp"

#include <cstddef>
#include <stdexcept>
#include <utility>

#include "core/money/rounding.hpp"

namespace core {

namespace {

const Decimal kHundred = Decimal::fromUnits(100);

const StepBar *firstSessionBar(const Timeline &Timeline, int Instrument,
                               std::int64_t LastStep) {
  if (const StepBar *AtZero = Timeline.barOf(Instrument, 0))
    return AtZero;
  for (std::int64_t Step = 1; Step <= LastStep; ++Step) {
    if (const StepBar *Bar = Timeline.barOf(Instrument, Step))
      return Bar;
  }
  return nullptr;
}

} // namespace

Decimal scaleBlindPrice(const Decimal &Price, const Decimal &P0) {
  if (P0 <= Decimal::zero())
    throw std::invalid_argument("P0 для слепого режима должен быть > 0");
  return Price.mulDiv(kHundred, P0, 2, RoundingMode::HalfUp);
}

Decimal scaleBlindVolume(std::int64_t VolumeLots, const BlindScale &Scale) {
  return Decimal::fromUnits(VolumeLots)
      .mulDiv(kHundred.times(Scale.VolCount), Decimal::fromUnits(Scale.VolSum),
              2, RoundingMode::HalfUp);
}

Decimal toPercentOfCapital(const Decimal &Amount, const Decimal &InitialCash) {
  if (InitialCash <= Decimal::zero())
    throw std::invalid_argument("начальный капитал должен быть > 0");
  return Amount.mulDiv(kHundred, InitialCash, 2, RoundingMode::HalfUp);
}

std::vector<BlindScale> computeBlindScales(const Timeline &Timeline,
                                           const Decimal &InitialCash,
                                           std::int64_t LastStep) {
  if (InitialCash <= Decimal::zero())
    throw std::invalid_argument("начальный капитал должен быть > 0");
  if (LastStep < 0 || LastStep >= Timeline.totalSteps())
    throw std::out_of_range("LastStep вне шкалы времени");

  const std::size_t NumInstruments = Timeline.instruments().size();
  std::vector<BlindScale> Scales(NumInstruments);
  std::vector<std::int64_t> WarmupVolSum(NumInstruments, 0);
  std::vector<std::int64_t> WarmupVolCount(NumInstruments, 0);

  for (const StepBar &Bar : Timeline.warmupBars()) {
    const std::size_t Idx = static_cast<std::size_t>(Bar.Instrument);
    WarmupVolSum[Idx] += Bar.VolumeLots;
    ++WarmupVolCount[Idx];
  }

  for (std::size_t Idx = 0; Idx < NumInstruments; ++Idx) {
    BlindScale &Scale = Scales[Idx];
    const StepBar *FirstBar =
        firstSessionBar(Timeline, static_cast<int>(Idx), LastStep);
    if (FirstBar != nullptr) {
      if (FirstBar->Open <= Decimal::zero()) {
        throw std::invalid_argument(
            "open первой свечи для слепого режима должен быть > 0");
      }
      Scale.P0 = FirstBar->Open;
      const Decimal OneLotCost =
          FirstBar->Open.times(Timeline.instruments()[Idx].LotSize);
      Scale.LotValueAtStart = toPercentOfCapital(OneLotCost, InitialCash);
    }

    if (WarmupVolCount[Idx] > 0) {
      if (WarmupVolSum[Idx] > 0) {
        Scale.VolSum = WarmupVolSum[Idx];
        Scale.VolCount = WarmupVolCount[Idx];
      } else {
        Scale.VolSum = 1;
        Scale.VolCount = 1;
      }
    } else if (FirstBar != nullptr && FirstBar->VolumeLots > 0) {
      Scale.VolSum = FirstBar->VolumeLots;
      Scale.VolCount = 1;
    } else {
      Scale.VolSum = 1;
      Scale.VolCount = 1;
    }
  }
  return Scales;
}

BlindBar toBlindBar(const StepBar &Bar, const BlindScale &Scale) {
  if (!Scale.P0)
    throw std::invalid_argument("нет P0 для масштабирования бара");
  BlindBar Out;
  Out.Step = Bar.Step;
  Out.Instrument = Bar.Instrument;
  Out.TradingDay = Bar.TradingDay;
  Out.Open = scaleBlindPrice(Bar.Open, *Scale.P0);
  Out.High = scaleBlindPrice(Bar.High, *Scale.P0);
  Out.Low = scaleBlindPrice(Bar.Low, *Scale.P0);
  Out.Close = scaleBlindPrice(Bar.Close, *Scale.P0);
  Out.VolumeRel = scaleBlindVolume(Bar.VolumeLots, Scale);
  Out.AfterHalt = Bar.AfterHalt;
  return Out;
}

BlindPortfolio toBlindPortfolio(const PortfolioSnapshot &Snapshot,
                                const Decimal &InitialCash,
                                const std::vector<BlindScale> &Scales) {
  BlindPortfolio Out;
  Out.CashPct = toPercentOfCapital(Snapshot.Cash, InitialCash);
  Out.EquityPct = toPercentOfCapital(Snapshot.Equity, InitialCash);
  Out.PnlPct = toPercentOfCapital(Snapshot.Equity - InitialCash, InitialCash);
  for (std::size_t Idx = 0; Idx < Snapshot.Lots.size(); ++Idx) {
    if (Snapshot.Lots[Idx] == 0)
      continue;
    const BlindScale &Scale = Scales.at(Idx);
    if (!Scale.P0)
      throw std::invalid_argument("нет P0 для открытой позиции");
    BlindPosition Pos;
    Pos.Instrument = static_cast<int>(Idx);
    Pos.Lots = Snapshot.Lots[Idx];
    Pos.AvgPrice = scaleBlindPrice(Snapshot.AvgPrice[Idx], *Scale.P0);
    Pos.MarketValuePct =
        toPercentOfCapital(Snapshot.MarketValue[Idx], InitialCash);
    Out.Positions.push_back(std::move(Pos));
  }
  return Out;
}

BlindOrder toBlindOrder(const Order &Order, const BlindScale &Scale) {
  BlindOrder Out;
  Out.Id = Order.Id;
  Out.Instrument = Order.Spec.Instrument;
  if (Order.Spec.LimitPrice) {
    if (!Scale.P0)
      throw std::invalid_argument("нет P0 для limitPrice заявки");
    Out.LimitPrice = scaleBlindPrice(*Order.Spec.LimitPrice, *Scale.P0);
  }
  if (Order.Spec.StopPrice) {
    if (!Scale.P0)
      throw std::invalid_argument("нет P0 для stopPrice заявки");
    Out.StopPrice = scaleBlindPrice(*Order.Spec.StopPrice, *Scale.P0);
  }
  if (Order.TrailingStopLevel) {
    if (!Scale.P0)
      throw std::invalid_argument("нет P0 для trailingStopLevel заявки");
    Out.TrailingStopLevel =
        scaleBlindPrice(*Order.TrailingStopLevel, *Scale.P0);
  }
  return Out;
}

BlindFill toBlindFill(const Fill &Fill, const Decimal &InitialCash,
                      const BlindScale &Scale) {
  if (!Scale.P0)
    throw std::invalid_argument("нет P0 для сделки");
  BlindFill Out;
  Out.OrderId = Fill.OrderId;
  Out.Step = Fill.Step;
  Out.Instrument = Fill.Instrument;
  Out.OrderSide = Fill.OrderSide;
  Out.Lots = Fill.Lots;
  Out.Price = scaleBlindPrice(Fill.Price, *Scale.P0);
  Out.BasePrice = scaleBlindPrice(Fill.BasePrice, *Scale.P0);
  Out.ValuePct = toPercentOfCapital(Fill.Value, InitialCash);
  Out.CommissionPct = toPercentOfCapital(Fill.Commission, InitialCash);
  Out.FeePct = toPercentOfCapital(Fill.Fee, InitialCash);
  Out.SpreadCostPct = toPercentOfCapital(Fill.SpreadCost, InitialCash);
  Out.SlippageCostPct = toPercentOfCapital(Fill.SlippageCost, InitialCash);
  Out.StopAndTakeSameBar = Fill.StopAndTakeSameBar;
  return Out;
}

BlindStepEvents toBlindStepEvents(const StepEvents &Events,
                                  const Decimal &InitialCash,
                                  const std::vector<BlindScale> &Scales) {
  BlindStepEvents Out;
  Out.Step = Events.Step;
  Out.Bars.reserve(Events.Bars.size());
  for (const StepBar &Bar : Events.Bars) {
    Out.Bars.push_back(
        toBlindBar(Bar, Scales.at(static_cast<std::size_t>(Bar.Instrument))));
  }
  Out.Fills.reserve(Events.Fills.size());
  for (const Fill &F : Events.Fills) {
    Out.Fills.push_back(toBlindFill(
        F, InitialCash, Scales.at(static_cast<std::size_t>(F.Instrument))));
  }
  Out.ChangedOrders.reserve(Events.ChangedOrders.size());
  for (const Order &O : Events.ChangedOrders) {
    Out.ChangedOrders.push_back(toBlindOrder(
        O, Scales.at(static_cast<std::size_t>(O.Spec.Instrument))));
  }
  Out.Portfolio = toBlindPortfolio(Events.Portfolio, InitialCash, Scales);
  return Out;
}

BlindReport toBlindReport(const Report &Report, const Decimal &InitialCash,
                          const std::vector<BlindScale> &Scales) {
  BlindReport Out;
  Out.Costs.CommissionPct =
      toPercentOfCapital(Report.Costs.Commission, InitialCash);
  Out.Costs.FeePct = toPercentOfCapital(Report.Costs.Fee, InitialCash);
  Out.Costs.SpreadPct = toPercentOfCapital(Report.Costs.Spread, InitialCash);
  Out.Costs.SlippagePct =
      toPercentOfCapital(Report.Costs.Slippage, InitialCash);

  Out.Equity.reserve(Report.Equity.size());
  for (const EquityPoint &P : Report.Equity) {
    Out.Equity.push_back(
        BlindEquityPoint{P.Step, toPercentOfCapital(P.Equity, InitialCash)});
  }
  Out.FinalEquityPct = toPercentOfCapital(Report.FinalEquity, InitialCash);
  Out.PnlPct =
      toPercentOfCapital(Report.FinalEquity - InitialCash, InitialCash);

  Out.BenchmarkEquity.reserve(Report.BenchmarkEquity.size());
  for (const EquityPoint &P : Report.BenchmarkEquity) {
    Out.BenchmarkEquity.push_back(
        BlindEquityPoint{P.Step, toPercentOfCapital(P.Equity, InitialCash)});
  }

  Out.RoundTrips.reserve(Report.RoundTrips.size());
  for (const RoundTrip &T : Report.RoundTrips) {
    const BlindScale &Scale = Scales.at(static_cast<std::size_t>(T.Instrument));
    if (!Scale.P0)
      throw std::invalid_argument("нет P0 для пары FIFO");
    BlindRoundTrip Trip;
    Trip.Instrument = T.Instrument;
    Trip.EntryStep = T.EntryStep;
    Trip.ExitStep = T.ExitStep;
    Trip.Lots = T.Lots;
    Trip.EntryPrice = scaleBlindPrice(T.EntryPrice, *Scale.P0);
    Trip.ExitPrice = scaleBlindPrice(T.ExitPrice, *Scale.P0);
    Trip.GrossPnlPct =
        toPercentOfCapital(T.ExitValue - T.EntryValue, InitialCash);
    Trip.CostsPct = toPercentOfCapital(T.Costs, InitialCash);
    Trip.NetPnlPct = toPercentOfCapital(T.NetPnl, InitialCash);
    Out.RoundTrips.push_back(std::move(Trip));
  }
  return Out;
}

OrderSpec convertBlindOrder(const OrderSpec &Spec, const Timeline &Timeline,
                            const std::vector<Instrument> &Instruments,
                            std::int64_t UpToStep) {
  if (!Spec.IsBlindPrice)
    return Spec;

  OrderSpec Out = Spec;
  if (Spec.Instrument < 0 ||
      static_cast<std::size_t>(Spec.Instrument) >= Instruments.size())
    return Out;

  const StepBar *FirstBar =
      firstSessionBar(Timeline, Spec.Instrument, UpToStep);
  if (FirstBar == nullptr || FirstBar->Open <= Decimal::zero())
    return Out;

  const Decimal &P0 = FirstBar->Open;
  const Decimal &PriceStep =
      Instruments[static_cast<std::size_t>(Spec.Instrument)].PriceStep;
  const RoundingMode Mode =
      Spec.OrderSide == Side::Buy ? RoundingMode::CEILING : RoundingMode::FLOOR;

  if (Out.LimitPrice && *Out.LimitPrice > Decimal::zero()) {
    Out.LimitPrice = Out.LimitPrice->mulDiv(P0, kHundred, Decimal::kScale, Mode)
                         .roundToStep(PriceStep, Mode);
  }
  if (Out.StopPrice && *Out.StopPrice > Decimal::zero()) {
    Out.StopPrice = Out.StopPrice->mulDiv(P0, kHundred, Decimal::kScale, Mode)
                        .roundToStep(PriceStep, Mode);
  }
  Out.IsBlindPrice = false;
  return Out;
}

} // namespace core
