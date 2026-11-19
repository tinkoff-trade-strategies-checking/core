#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/market/calendar_source.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"

// Безрисковая ставка RUONIA для отчёта (ФТ-17, documentation#40, A7, core#26).
// Дневная ставка в % годовых (например, 15.25 для 15,25 %).
// Конвенция Actual/365 по календарным дням (`decisions.md`, «Метрики отчёта»):
// r_{f,d} = RUONIA_{d-1} / 100 * Delta_d / 365, где Delta_d - календарные дни
// от предыдущего торгового дня d-1 до дня d.

namespace core {

struct DailyRate {
  std::int32_t Yyyymmdd = 0; // дата по Москве
  Decimal Rate;              // ставка в % годовых

  friend bool operator==(const DailyRate &, const DailyRate &) = default;
};

// Дневной ряд ставок RUONIA по возрастанию даты.
struct RuoniaSeries {
  std::vector<DailyRate> Rates;
};

// Календарных дней между двумя московскими датами (yyyymmdd).
std::int32_t calendarDaysBetween(std::int32_t FromYmd, std::int32_t ToYmd);

// Дата через n календарных дней после ymd (n может быть отрицательным).
std::int32_t addCalendarDays(std::int32_t Ymd, std::int32_t Days);

// Результат сопоставления: ставки RUONIA_{d-1} и календарные дни Delta_d.
struct MatchedRuonia {
  std::vector<Decimal> Rates; // RUONIA_{d-1} (% годовых) на каждый день
  std::vector<std::int32_t>
      CalendarDays; // Delta_d календарных дней для каждого дня
};

// Сопоставляет дни сессии со ставками RUONIA с forward fill и днями Actual/365:
// Для каждого торгового дня i из DayDates (i = 0…N-1):
//  - предыдущая дата: для i > 0 - DayDates[i - 1]; для i == 0 - PreviousDayYmd
//    (если 0 или не раньше DayDates[0] - предыдущего торгового дня в данных
//    нет: Delta_0 = 1, ставка берётся на календарный день DayDates[0] - 1,
//    то есть последняя известная ставка <= d-1, но не ставка дня d);
//  - Delta_i = calendarDaysBetween(PrevDate, DayDates[i]) (>= 1);
//  - ставка RUONIA_{i-1}: ставка на дату PrevDate (с forward fill назад);
//  - если до этой даты ставок нет совсем - MarketDataError.
// PreviousDayYmd >= DayDates[0] (при > 0) - std::invalid_argument.
MatchedRuonia matchRuoniaRates(const RuoniaSeries &Series,
                               const std::vector<std::int32_t> &DayDates,
                               std::int32_t PreviousDayYmd = 0);

// Даты берутся по всем торговым дням шкалы 0…LastStep включительно, в том
// числе неполному последнему дню (согласовано с dailySeries в metrics.cpp;
// для каждого дня d берётся ставка предыдущего дня RUONIA_{d-1}, INV-1).
// LastStep вне шкалы - std::invalid_argument. Предыдущий торговый день перед
// первым днём - PreviousDayYmd (0 - его нет, фолбэк выше); вариант без него
// берёт последний день прогрева шкалы (previousDayYmd), если он раньше первого
// дня, иначе 0.
MatchedRuonia matchRuoniaRates(const RuoniaSeries &Series,
                               const Timeline &Timeline, std::int64_t LastStep,
                               std::int32_t PreviousDayYmd);
MatchedRuonia matchRuoniaRates(const RuoniaSeries &Series,
                               const Timeline &Timeline, std::int64_t LastStep);

// Последний торговый день календаря версии данных раньше FirstYmd (окно поиска
// растёт до года: 31…366 дней); 0, если в окне торговых дней нет. INV-1: только
// прошлое.
std::int32_t findPreviousTradingDay(CalendarSource &Calendar,
                                    const std::string &SnapshotId,
                                    std::int32_t FirstYmd);

// Торговый день перед первым днём шкалы: findPreviousTradingDay по календарю
// снапшота; если не найден — последний день прогрева шкалы (previousDayYmd),
// если он раньше первого дня; иначе 0. INV-1: только прошлое.
std::int32_t previousTradingDay(CalendarSource &Calendar,
                                const std::string &SnapshotId,
                                const Timeline &Timeline);

} // namespace core
