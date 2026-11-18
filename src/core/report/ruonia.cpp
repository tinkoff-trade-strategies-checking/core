#include "core/report/ruonia.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>

#include "core/market/bar_source.hpp" // MarketDataError

namespace core {

namespace {

constexpr std::int64_t daysFromCivil(std::int32_t Y, std::int32_t M,
                                     std::int32_t D) {
  Y -= M <= 2 ? 1 : 0;
  const std::int64_t Era = (Y >= 0 ? Y : Y - 399) / 400;
  const std::int64_t Yoe = Y - Era * 400;
  const std::int64_t Doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
  const std::int64_t Doe = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;
  return Era * 146097 + Doe - 719468;
}

std::int64_t ymdToDays(std::int32_t Ymd) {
  const int Y = Ymd / 10000;
  const int M = Ymd / 100 % 100;
  const int D = Ymd % 100;
  return daysFromCivil(Y, M, D);
}

} // namespace

std::int32_t calendarDaysBetween(std::int32_t FromYmd, std::int32_t ToYmd) {
  return static_cast<std::int32_t>(ymdToDays(ToYmd) - ymdToDays(FromYmd));
}

MatchedRuonia matchRuoniaRates(const RuoniaSeries &Series,
                               const std::vector<std::int32_t> &DayDates,
                               std::int32_t PreviousDayYmd) {
  if (DayDates.empty())
    return {};
  if (Series.Rates.empty())
    throw MarketDataError("нет ставок RUONIA для периода");

  MatchedRuonia Out;
  Out.Rates.reserve(DayDates.size());
  Out.CalendarDays.reserve(DayDates.size());

  for (std::size_t I = 0; I < DayDates.size(); ++I) {
    const std::int32_t Date = DayDates[I];
    std::int32_t PrevDate = 0;
    std::int32_t DeltaDays = 1;

    if (I > 0) {
      PrevDate = DayDates[I - 1];
      DeltaDays = calendarDaysBetween(PrevDate, Date);
      if (DeltaDays <= 0)
        DeltaDays = 1;
    } else if (PreviousDayYmd > 0 && PreviousDayYmd < Date) {
      PrevDate = PreviousDayYmd;
      DeltaDays = calendarDaysBetween(PrevDate, Date);
      if (DeltaDays <= 0)
        DeltaDays = 1;
    } else {
      PrevDate = Date;
      DeltaDays = 1;
    }

    auto It = std::upper_bound(
        Series.Rates.begin(), Series.Rates.end(), PrevDate,
        [](std::int32_t Val, const DailyRate &R) { return Val < R.Yyyymmdd; });
    if (It == Series.Rates.begin()) {
      throw MarketDataError("нет ставки RUONIA до даты " +
                            std::to_string(PrevDate));
    }
    --It;
    Out.Rates.push_back(It->Rate);
    Out.CalendarDays.push_back(DeltaDays);
  }

  return Out;
}

MatchedRuonia matchRuoniaRates(const RuoniaSeries &Series,
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

  return matchRuoniaRates(Series, DayDates, 0);
}

} // namespace core
