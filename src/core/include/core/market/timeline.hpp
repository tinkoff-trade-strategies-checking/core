#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/market/bar.hpp"
#include "core/market/bar_source.hpp"
#include "core/market/calendar_source.hpp"
#include "core/market/instrument.hpp"
#include "core/market/step_bar.hpp"
#include "core/market/timeframe.hpp"

namespace core {

// Общая шкала времени сессии: отсортированное объединение моментов начала баров всех бумаг,
// шаги с 0. У бумаги на шаге бар есть, только если у неё есть бар с этим временем.
//
// Timeline хранит всю шкалу и сама INV-1 не обеспечивает: видимость бара определяет курсор
// движка (бар шага виден, только когда курсор дошёл до шага, ФТ-5). Номера шагов вне
// [0, totalSteps) - std::out_of_range.
class Timeline {
 public:
  // bars[i] - бары instruments[i] по возрастанию tsOpenMs (иначе std::invalid_argument);
  // calendar - дни календаря (нужен для afterHalt); index баров не используется.
  static Timeline build(std::vector<Instrument> instruments, const std::vector<std::vector<Bar>>& bars,
                        const std::vector<TradingDay>& calendar);
  // Читает бары каждой бумаги (по ticker) за московские даты fromYmd…toYmd включительно.
  static Timeline load(BarSource& barSource, CalendarSource& calendarSource, const std::string& snapshotId,
                       std::vector<Instrument> instruments, Timeframe timeframe, std::int32_t fromYmd,
                       std::int32_t toYmd);

  std::int64_t totalSteps() const { return static_cast<std::int64_t>(stepStart_.size()) - 1; }
  // Бары шага по возрастанию instrument.
  std::span<const StepBar> barsAt(std::int64_t step) const;
  // nullptr, если у бумаги на шаге нет бара.
  const StepBar* barOf(int instrument, std::int64_t step) const;
  // Последний бар бумаги на шагах 0…upToStep; nullptr, если баров ещё не было.
  const StepBar* lastBarOf(int instrument, std::int64_t upToStep) const;
  std::int32_t tradingDayOf(std::int64_t step) const;  // 1…N
  std::int32_t yyyymmddOf(std::int64_t step) const;
  bool isLastStepOfDay(std::int64_t step) const;
  std::int32_t tradingDays() const { return tradingDays_; }
  // Торговые дни fromDay…toDay (1…N, включительно); шаги и дни перенумерованы с 0 и с 1.
  Timeline slice(std::int32_t fromDay, std::int32_t toDay) const;
  const std::vector<Instrument>& instruments() const { return instruments_; }
  // Бар для правки - только для стресс-сценария. std::out_of_range, если бара нет.
  StepBar& mutableBar(std::int64_t step, int instrument);

 private:
  Timeline() = default;
  static Timeline buildWithInterval(std::vector<Instrument> instruments, const std::vector<std::vector<Bar>>& bars,
                                    const std::vector<TradingDay>& calendar, std::int64_t intervalMs);
  void checkStep(std::int64_t step) const;
  void checkInstrument(int instrument) const;
  void buildIndex();

  std::vector<Instrument> instruments_;
  std::vector<StepBar> bars_;                       // по возрастанию (step, instrument)
  std::vector<std::size_t> stepStart_;              // totalSteps + 1 смещений в bars_
  std::vector<std::int32_t> stepDay_;               // торговый день шага
  std::vector<std::int32_t> stepYmd_;               // московская дата шага
  std::vector<std::vector<std::size_t>> byInstrument_;  // индексы в bars_ по возрастанию шага
  std::int32_t tradingDays_ = 0;
};

}  // namespace core
