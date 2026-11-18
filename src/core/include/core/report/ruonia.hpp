#pragma once

#include <cstdint>
#include <vector>

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

// Результат сопоставления: ставки RUONIA_{d-1} и календарные дни Delta_d.
struct MatchedRuonia {
  std::vector<Decimal> Rates; // RUONIA_{d-1} (% годовых) на каждый день
  std::vector<std::int32_t>
      CalendarDays; // Delta_d календарных дней для каждого дня
};

// Сопоставляет дни сессии со ставками RUONIA с forward fill и днями Actual/365:
// Для каждого торгового дня i из DayDates (i = 0…N-1):
//  - предыдущая дата: для i > 0 - DayDates[i - 1]; для i == 0 - PreviousDayYmd
//    (если > 0, иначе Delta_0 = 1);
//  - Delta_i = calendarDaysBetween(PrevDate, DayDates[i]);
//  - ставка RUONIA_{i-1}: ставка на дату PrevDate (с forward fill назад);
//  - если до этой даты ставок нет совсем - MarketDataError.
MatchedRuonia matchRuoniaRates(const RuoniaSeries &Series,
                               const std::vector<std::int32_t> &DayDates,
                               std::int32_t PreviousDayYmd = 0);

// Даты берутся по последнему шагу каждого полностью завершённого торгового дня
// шкалы 0…LastStep (INV-1: день, в котором сессия остановилась посреди дня,
// пропускается, как в benchmarkEquity).
// LastStep вне шкалы - std::invalid_argument.
MatchedRuonia matchRuoniaRates(const RuoniaSeries &Series,
                               const Timeline &Timeline, std::int64_t LastStep);

} // namespace core
