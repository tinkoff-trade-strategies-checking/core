#include "core/report/derived.hpp"

#include <stdexcept>

#include "core/money/rounding.hpp"

namespace core {

namespace {

const Decimal kHundred = Decimal::fromUnits(100);

// Процент Part/Whole*100, 2 знака HALF_UP; Whole <= 0 - 0.
Decimal pct(const Decimal &Part, const Decimal &Whole) {
  if (Whole <= Decimal::zero())
    return Decimal::zero();
  return Part.mulDiv(kHundred, Whole, 2, RoundingMode::HalfUp);
}

} // namespace

Decimal cumulativeCosts(const std::vector<Fill> &Fills, std::int64_t UpToStep) {
  Decimal Sum;
  for (const Fill &F : Fills)
    if (F.Step <= UpToStep)
      Sum += F.cost();
  return Sum;
}

PortfolioPcts portfolioPcts(const PortfolioSnapshot &Snapshot,
                            const Decimal &InitialCash,
                            const Decimal &CumulativeCosts) {
  if (InitialCash <= Decimal::zero())
    throw std::invalid_argument("начальный капитал должен быть > 0");
  Decimal Invested;
  for (const Decimal &V : Snapshot.MarketValue)
    Invested += V;
  PortfolioPcts Out;
  Out.InvestedPct = pct(Invested, Snapshot.Equity);
  Out.CostsPct = pct(CumulativeCosts, InitialCash);
  Out.PnlPct = pct(Snapshot.Equity - InitialCash, InitialCash);
  return Out;
}

Decimal positionSharePct(const Decimal &MarketValue, const Decimal &Equity) {
  return pct(MarketValue, Equity);
}

Decimal positionPnlPct(const Decimal &MarketValue, const Decimal &AvgPrice,
                       std::int64_t Lots, std::int64_t LotSize) {
  const Decimal Basis = AvgPrice.times(Lots).times(LotSize);
  return pct(MarketValue - Basis, Basis);
}

InstrumentChange instrumentChange(const Timeline &Timeline, int Instrument,
                                  std::int64_t LastStep) {
  InstrumentChange Out;
  const StepBar *Last = Timeline.lastBarOf(Instrument, LastStep);
  if (Last == nullptr)
    return Out;

  // Предыдущий close: бары до Last по шагам, затем прогрев.
  std::optional<Decimal> PrevClose;
  for (std::int64_t Step = Last->Step - 1; Step >= 0 && !PrevClose; --Step)
    if (const StepBar *B = Timeline.barOf(Instrument, Step))
      PrevClose = B->Close;
  if (!PrevClose) {
    for (const StepBar &B : Timeline.warmupBars())
      if (B.Instrument == Instrument)
        PrevClose = B.Close; // по возрастанию шага: останется последний
  }
  const Decimal &Base = PrevClose ? *PrevClose : Last->Open;
  if (Base > Decimal::zero())
    Out.StepPct = pct(Last->Close - Base, Base);

  for (std::int64_t Step = 0; Step <= Last->Step; ++Step) {
    if (const StepBar *First = Timeline.barOf(Instrument, Step)) {
      if (First->Open > Decimal::zero())
        Out.PeriodPct = pct(Last->Close - First->Open, First->Open);
      break;
    }
  }
  return Out;
}

} // namespace core
