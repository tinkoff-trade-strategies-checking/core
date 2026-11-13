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
  // load и прогрев: до warmupSteps шагов истории по датам раньше fromYmd (меньше, если в снапшоте раньше столько баров
  // нет). Окно поиска истории начинается с оценки по числу шагов в торговом дне и растёт вдвое, пока шагов не хватает,
  // но не дальше kMaxWarmupLookbackDays календарных дней (хватает на 5000 дневных баров). warmupSteps <= 0 или пустой
  // период - как load.
  static constexpr std::int64_t kMaxWarmupLookbackDays = 7500;
  static Timeline loadWithWarmup(BarSource& barSource, CalendarSource& calendarSource, const std::string& snapshotId,
                                 std::vector<Instrument> instruments, Timeframe timeframe, std::int32_t fromYmd,
                                 std::int32_t toYmd, std::int64_t warmupSteps);

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
  // Торговые дни fromDay…toDay (1…N, включительно); шаги и дни перенумерованы с 0 и с 1. Прогрев среза - те же W
  // шагов, что у шкалы, взятые непосредственно перед fromDay (из дней шкалы и её прогрева; меньше, если раньше
  // столько шагов нет).
  Timeline slice(std::int32_t fromDay, std::int32_t toDay) const;
  const std::vector<Instrument>& instruments() const { return instruments_; }
  // Бар для правки - только для стресс-сценария. std::out_of_range, если бара нет.
  StepBar& mutableBar(std::int64_t step, int instrument);

  // История до начала шкалы - прогрев индикаторов (`MarketSpec.warmup_bars`). Шаги прогрева -W…-1, торговые дни
  // <= 0 (последний день истории - 0, день перед ним - -1, ...). Торговли и кривой стоимости на них нет; это прошлое
  // относительно шага 0, поэтому стратегия и человек видят их с самого начала (INV-1). Бары - по возрастанию
  // (step, instrument). Без прогрева - пусто, W = 0.
  std::int64_t warmupSteps() const { return warmupSteps_; }
  std::span<const StepBar> warmupBars() const { return warmup_; }
  // Копия шкалы с прогревом: последние steps шагов history (меньше, если в history меньше шагов). Бумаги history - те
  // же, что у шкалы (ключи по порядку), и все бары history раньше первого бара шкалы - иначе std::invalid_argument.
  // steps < 0 - std::invalid_argument; прежний прогрев заменяется.
  Timeline withWarmup(const Timeline& history, std::int64_t steps) const;

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
  std::vector<StepBar> warmup_;  // шаги -warmupSteps_…-1, по возрастанию (step, instrument)
  std::int64_t warmupSteps_ = 0;
};

}  // namespace core
