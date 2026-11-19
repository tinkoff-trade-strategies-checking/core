#include "core/report/derived.hpp"

#include <algorithm>
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

CostLedger::CostLedger(const std::vector<Fill> &Fills) {
  Prefix.push_back(Decimal::zero());
  for (const Fill &F : Fills) {
    Steps.push_back(F.Step);
    Prefix.push_back(Prefix.back() + F.cost());
  }
}

Decimal CostLedger::upTo(std::int64_t Step) const {
  const auto End = std::upper_bound(Steps.begin(), Steps.end(), Step);
  return Prefix[static_cast<std::size_t>(End - Steps.begin())];
}

PortfolioPcts portfolioPcts(const PortfolioSnapshot &Snapshot,
                            const Decimal &InitialCash,
                            const Decimal &CumulativeCosts) {
  if (InitialCash <= Decimal::zero())
    throw std::invalid_argument("начальный капитал должен быть > 0");
  PortfolioPcts Out;
  for (const Decimal &V : Snapshot.MarketValue)
    Out.Invested += V;
  Out.Pnl = Snapshot.Equity - InitialCash;
  Out.Costs = CumulativeCosts;
  Out.EquityPct = pct(Snapshot.Equity, InitialCash);
  Out.CashPct = pct(Snapshot.Cash, InitialCash);
  Out.InvestedPct = pct(Out.Invested, InitialCash);
  Out.PnlPct = pct(Out.Pnl, InitialCash);
  Out.CostsPct = pct(CumulativeCosts, InitialCash);
  return Out;
}

Decimal positionValuePct(const Decimal &MarketValue,
                         const Decimal &InitialCash) {
  return pct(MarketValue, InitialCash);
}

Decimal positionPnlPct(const Decimal &MarketValue, const Decimal &CostBasis,
                       const Decimal &InitialCash) {
  return pct(MarketValue - CostBasis, InitialCash);
}

InstrumentChange instrumentChange(const Timeline &Timeline, int Instrument,
                                  std::int64_t LastStep) {
  InstrumentChange Out;
  const StepBar *Last = Timeline.lastBarOf(Instrument, LastStep);
  if (Last == nullptr)
    return Out;

  // Предыдущий close: бар до Last по шагам, затем прогрев.
  std::optional<Decimal> PrevClose;
  if (Last->Step >= 1) {
    if (const StepBar *Prev = Timeline.lastBarOf(Instrument, Last->Step - 1))
      PrevClose = Prev->Close;
  }
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
