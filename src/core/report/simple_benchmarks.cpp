#include "core/report/simple_benchmarks.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>

#include "core/market/bar_source.hpp" // MarketDataError
#include "core/money/rounding.hpp"

namespace core {
namespace {

void checkArgs(const Timeline &Timeline, std::int64_t LastStep,
               const Decimal &InitialCash) {
  if (InitialCash <= Decimal::zero())
    throw std::invalid_argument("simple benchmarks: initialCash <= 0");
  if (LastStep < 0 || LastStep >= Timeline.totalSteps())
    throw std::invalid_argument("simple benchmarks: lastStep вне шкалы");
}

struct Entry {
  std::int64_t Lots = 0;
  Decimal Total; // Value + Commission + Fee
};

// Максимум лотов, которые влезают в бюджет вместе с издержками. Цена зависит от
// числа лотов (проскальзывание), поэтому число лотов уменьшается, пока
// стоимость не поместится в бюджет; каждый шаг строго уменьшает его.
Entry affordableEntry(const Decimal &Budget, const StepBar &Bar,
                      const Instrument &Paper, const CostConfig &Costs) {
  const Decimal LotValue = Bar.Open.times(Paper.LotSize);
  if (LotValue <= Decimal::zero() || Budget <= Decimal::zero())
    return {};
  std::int64_t Lots = static_cast<std::int64_t>(
      Budget.div(LotValue, 0, RoundingMode::DOWN).raw() / Decimal::kUnit);
  while (Lots > 0) {
    const Decimal Price = fillPrice(Side::Buy, Bar.Open, Lots, Bar.VolumeLots,
                                    Costs, Paper.PriceStep);
    const Amounts A = amounts(Price, Lots, Paper.LotSize, Costs);
    const Decimal Total = A.Value + A.Commission + A.Fee;
    if (Total <= Budget)
      return Entry{Lots, Total};
    const std::int64_t Scaled = static_cast<std::int64_t>(
        Decimal::fromUnits(Lots)
            .mulDiv(Budget, Total, 0, RoundingMode::DOWN)
            .raw() /
        Decimal::kUnit);
    Lots = std::min(Lots - 1, Scaled);
  }
  return {};
}

} // namespace

std::vector<EquityPoint> buyHoldEquity(const Timeline &Timeline,
                                       std::int64_t LastStep,
                                       const Decimal &InitialCash,
                                       const CostConfig &Costs) {
  checkArgs(Timeline, LastStep, InitialCash);
  const std::vector<Instrument> &Papers = Timeline.instruments();
  if (Papers.empty())
    return {};
  for (const Instrument &Paper : Papers) {
    if (Paper.PriceStep <= Decimal::zero() || Paper.LotSize <= 0)
      return {};
  }

  const Decimal Budget = InitialCash.div(
      Decimal::fromUnits(static_cast<std::int64_t>(Papers.size())), 2,
      RoundingMode::DOWN);
  std::vector<bool> Entered(Papers.size(), false);
  std::vector<std::int64_t> Lots(Papers.size(), 0);
  Decimal Cash = InitialCash;

  std::vector<EquityPoint> Out;
  for (std::int64_t Step = 0; Step <= LastStep; ++Step) {
    for (const StepBar &Bar : Timeline.barsAt(Step)) {
      const auto Idx = static_cast<std::size_t>(Bar.Instrument);
      if (Entered[Idx])
        continue;
      Entered[Idx] = true;
      const Entry E = affordableEntry(Budget, Bar, Papers[Idx], Costs);
      Lots[Idx] = E.Lots;
      Cash -= E.Total;
    }
    if (!Timeline.isLastStepOfDay(Step))
      continue;
    Decimal Equity = Cash;
    for (std::size_t I = 0; I < Papers.size(); ++I) {
      if (Lots[I] == 0)
        continue;
      const StepBar *Last = Timeline.lastBarOf(static_cast<int>(I), Step);
      if (Last == nullptr)
        continue;
      Equity +=
          Portfolio::positionValue(Lots[I], Papers[I].LotSize, Last->Close);
    }
    Out.push_back(EquityPoint{Step, Equity});
  }
  return Out;
}

std::vector<EquityPoint> ruoniaEquity(const RuoniaSeries &Series,
                                      const Timeline &Timeline,
                                      std::int64_t LastStep,
                                      const Decimal &InitialCash,
                                      std::int32_t PreviousDayYmd) {
  checkArgs(Timeline, LastStep, InitialCash);
  const MatchedRuonia Matched =
      matchRuoniaRates(Series, Timeline, LastStep, PreviousDayYmd);
  const Decimal Basis = Decimal::fromUnits(36500); // 100 * 365

  std::vector<EquityPoint> Out;
  Decimal Equity = InitialCash;
  std::size_t DayIdx = 0;
  std::int32_t CurrentDay = 0;
  for (std::int64_t Step = 0; Step <= LastStep; ++Step) {
    const std::int32_t Day = Timeline.tradingDayOf(Step);
    if (Step == 0 || Day != CurrentDay) {
      CurrentDay = Day;
      const Decimal Factor =
          Matched.Rates.at(DayIdx).times(Matched.CalendarDays.at(DayIdx));
      Equity +=
          Equity.mulDiv(Factor, Basis, Decimal::kScale, RoundingMode::HalfUp);
      ++DayIdx;
    }
    if (Timeline.isLastStepOfDay(Step))
      Out.push_back(EquityPoint{Step, Equity.round(2, RoundingMode::HalfUp)});
  }
  return Out;
}

std::optional<Decimal> returnPct(const std::vector<EquityPoint> &Curve,
                                 const Decimal &InitialCash) {
  if (Curve.empty() || InitialCash <= Decimal::zero())
    return std::nullopt;
  return (Curve.back().Equity - InitialCash)
      .mulDiv(Decimal::fromUnits(100), InitialCash, 2, RoundingMode::HalfUp);
}

SimpleBenchmarks
buildSimpleBenchmarks(const Timeline &Timeline, std::int64_t LastStep,
                      const Decimal &InitialCash, const CostConfig &Costs,
                      const std::optional<RuoniaSeries> &Ruonia,
                      std::int32_t PreviousDayYmd) {
  SimpleBenchmarks Out;
  Out.BuyHoldEquity = buyHoldEquity(Timeline, LastStep, InitialCash, Costs);
  Out.BuyHoldReturnPct = returnPct(Out.BuyHoldEquity, InitialCash);
  if (Ruonia && !Ruonia->Rates.empty()) {
    try {
      Out.RuoniaEquity = ruoniaEquity(*Ruonia, Timeline, LastStep, InitialCash,
                                      PreviousDayYmd);
      Out.RuoniaReturnPct = returnPct(Out.RuoniaEquity, InitialCash);
    } catch (const MarketDataError &) {
      Out.RuoniaEquity.clear();
      Out.RuoniaReturnPct.reset();
    }
  }
  return Out;
}

void applySimpleBenchmarks(Report &Report, SimpleBenchmarks Benchmarks) {
  Report.BuyHoldEquity = std::move(Benchmarks.BuyHoldEquity);
  Report.RuoniaEquity = std::move(Benchmarks.RuoniaEquity);
  Report.BuyHoldReturnPct = Benchmarks.BuyHoldReturnPct;
  Report.RuoniaReturnPct = Benchmarks.RuoniaReturnPct;
}

} // namespace core
