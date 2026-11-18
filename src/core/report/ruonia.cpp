#include "core/report/ruonia.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>

#include "core/market/bar_source.hpp" // MarketDataError

namespace core {

std::vector<Decimal>
matchRuoniaRates(const RuoniaSeries &Series,
                 const std::vector<std::int32_t> &DayDates) {
  if (DayDates.empty())
    return {};
  if (Series.Rates.empty())
    throw MarketDataError("нет ставок RUONIA для периода");

  std::vector<Decimal> Out;
  Out.reserve(DayDates.size());

  for (std::int32_t Date : DayDates) {
    auto It = std::upper_bound(
        Series.Rates.begin(), Series.Rates.end(), Date,
        [](std::int32_t Val, const DailyRate &R) { return Val < R.Yyyymmdd; });
    if (It == Series.Rates.begin()) {
      throw MarketDataError("нет ставки RUONIA до даты " +
                            std::to_string(Date));
    }
    --It;
    Out.push_back(It->Rate);
  }

  return Out;
}

std::vector<Decimal> matchRuoniaRates(const RuoniaSeries &Series,
                                      const Timeline &Timeline,
                                      std::int64_t LastStep) {
  if (LastStep < 0 || LastStep >= Timeline.totalSteps())
    throw std::invalid_argument("lastStep вне шкалы");

  std::vector<std::int32_t> DayDates;
  std::int32_t CurrentDay = 0;
  for (std::int64_t Step = 0; Step <= LastStep; ++Step) {
    const std::int32_t Day = Timeline.tradingDayOf(Step);
    if (Step == 0 || Day != CurrentDay) {
      DayDates.push_back(Timeline.yyyymmddOf(Step));
      CurrentDay = Day;
    }
  }

  return matchRuoniaRates(Series, DayDates);
}

} // namespace core
