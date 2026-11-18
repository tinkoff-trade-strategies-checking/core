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

std::int32_t civilFromDays(std::int64_t Z) {
  Z += 719468;
  const std::int64_t Era = (Z >= 0 ? Z : Z - 146096) / 146097;
  const std::int64_t Doe = Z - Era * 146097;
  const std::int64_t Yoe =
      (Doe - Doe / 1460 + Doe / 36524 - Doe / 146096) / 365;
  const std::int64_t Doy = Doe - (365 * Yoe + Yoe / 4 - Yoe / 100);
  const std::int64_t Mp = (5 * Doy + 2) / 153;
  const std::int64_t D = Doy - (153 * Mp + 2) / 5 + 1;
  const std::int64_t M = Mp < 10 ? Mp + 3 : Mp - 9;
  const std::int64_t Y = Yoe + Era * 400 + (M <= 2 ? 1 : 0);
  return static_cast<std::int32_t>(Y * 10000 + M * 100 + D);
}

} // namespace

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

} // namespace core
