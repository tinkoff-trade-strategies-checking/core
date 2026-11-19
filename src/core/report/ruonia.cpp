#include "core/report/ruonia.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>

#include "core/market/bar_source.hpp" // MarketDataError
#include "core/market/date_utils.hpp"

namespace core {

std::int32_t calendarDaysBetween(std::int32_t FromYmd, std::int32_t ToYmd) {
  return static_cast<std::int32_t>(ymdToDays(ToYmd) - ymdToDays(FromYmd));
}

std::int32_t addCalendarDays(std::int32_t Ymd, std::int32_t Days) {
  return civilFromDays(ymdToDays(Ymd) + Days);
}

MatchedRuonia matchRuoniaRates(const RuoniaSeries &Series,
                               const std::vector<std::int32_t> &DayDates,
                               std::int32_t PreviousDayYmd) {
  if (DayDates.empty())
    return {};
  if (Series.Rates.empty())
    throw MarketDataError("нет ставок RUONIA для периода");

  if (PreviousDayYmd > 0 && PreviousDayYmd >= DayDates.front())
    throw std::invalid_argument(
        "предыдущий торговый день не раньше первого дня периода");

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
    } else if (PreviousDayYmd > 0) {
      PrevDate = PreviousDayYmd;
      DeltaDays = calendarDaysBetween(PrevDate, Date);
      if (DeltaDays <= 0)
        DeltaDays = 1;
    } else {
      // Предыдущего торгового дня в данных нет: Delta = 1, ставка последнего
      // известного дня <= d-1 (никогда не ставка дня d, INV-1).
      PrevDate = civilFromDays(ymdToDays(Date) - 1);
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
  const std::int32_t Warmup = Timeline.previousDayYmd();
  return matchRuoniaRates(Series, Timeline, LastStep,
                          Warmup < Timeline.yyyymmddOf(0) ? Warmup : 0);
}

MatchedRuonia matchRuoniaRates(const RuoniaSeries &Series,
                               const Timeline &Timeline, std::int64_t LastStep,
                               std::int32_t PreviousDayYmd) {
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

  return matchRuoniaRates(Series, DayDates, PreviousDayYmd);
}

std::int32_t findPreviousTradingDay(CalendarSource &Calendar,
                                    const std::string &SnapshotId,
                                    std::int32_t FirstYmd) {
  const std::int32_t To = addCalendarDays(FirstYmd, -1);
  for (const std::int32_t Lookback : {31, 62, 124, 248, 366}) {
    const std::vector<TradingDay> Days = Calendar.tradingDays(
        SnapshotId, addCalendarDays(FirstYmd, -Lookback), To);
    for (auto It = Days.rbegin(); It != Days.rend(); ++It) {
      if (It->IsTrading)
        return It->Yyyymmdd;
    }
  }
  return 0;
}

std::int32_t previousTradingDay(CalendarSource &Calendar,
                                const std::string &SnapshotId,
                                const Timeline &Timeline) {
  const std::int32_t First = Timeline.yyyymmddOf(0);
  const std::int32_t Found =
      findPreviousTradingDay(Calendar, SnapshotId, First);
  if (Found > 0)
    return Found;
  const std::int32_t Warmup = Timeline.previousDayYmd();
  return Warmup < First ? Warmup : 0;
}

} // namespace core
