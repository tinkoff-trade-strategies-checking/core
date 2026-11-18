#pragma once

#include <cstdint>
#include <vector>

#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"

// Безрисковая ставка RUONIA для отчёта (ФТ-17, documentation#40, A7). Дневная
// ставка в % годовых (например, 15.25 для 15,25 %). Конвенция Actual/365:
// r_{f,d} = Rate / 100 / 365 (`decisions.md`, «Метрики отчёта»).

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

// Сопоставляет дни сессии со ставками RUONIA с forward fill:
// Для каждого торгового дня из DayDates:
//  - если на дату есть ставка в Series, берётся она;
//  - иначе берётся последняя известная ставка на предшествующую дату;
//  - если до этой даты ставок в Series нет вообще (или Series пуст) -
//    MarketDataError.
// Возвращает вектор ставок в % годовых длины DayDates.size().
std::vector<Decimal>
matchRuoniaRates(const RuoniaSeries &Series,
                 const std::vector<std::int32_t> &DayDates);

// Даты берутся по последнему шагу каждого полностью завершённого торгового дня
// шкалы 0…LastStep (INV-1: день, в котором сессия остановилась посреди дня,
// пропускается, как в benchmarkEquity).
// LastStep вне шкалы - std::invalid_argument.
std::vector<Decimal> matchRuoniaRates(const RuoniaSeries &Series,
                                      const Timeline &Timeline,
                                      std::int64_t LastStep);

} // namespace core
