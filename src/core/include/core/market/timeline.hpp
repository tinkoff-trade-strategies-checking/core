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

// Общая шкала времени сессии: отсортированное объединение моментов начала баров
// всех бумаг, шаги с 0. У бумаги на шаге бар есть, только если у неё есть бар с
// этим временем.
//
// Timeline хранит всю шкалу и сама INV-1 не обеспечивает: видимость бара
// определяет курсор движка (бар шага виден, только когда курсор дошёл до шага,
// ФТ-5). Номера шагов вне [0, totalSteps) - std::out_of_range.
class Timeline {
public:
  // bars[i] - бары instruments[i] по возрастанию tsOpenMs (иначе
  // std::invalid_argument); calendar - дни календаря (нужен для afterHalt);
  // index баров не используется.
  static Timeline build(std::vector<Instrument> Instruments,
                        const std::vector<std::vector<Bar>> &Bars,
                        const std::vector<TradingDay> &Calendar);
  // Читает бары каждой бумаги (по ticker) за московские даты fromYmd…toYmd
  // включительно.
  static Timeline load(BarSource &BarSource, CalendarSource &CalendarSource,
                       const std::string &SnapshotId,
                       std::vector<Instrument> Instruments, Timeframe Timeframe,
                       std::int32_t FromYmd, std::int32_t ToYmd);
  // load и прогрев: до warmupSteps шагов истории по датам раньше fromYmd
  // (меньше, если в снапшоте раньше столько баров нет). Окно поиска истории
  // начинается с оценки по числу шагов в торговом дне и растёт вдвое, пока
  // шагов не хватает, но не дальше kMaxWarmupLookbackDays календарных дней
  // (хватает на 5000 дневных баров). warmupSteps <= 0 или пустой период - как
  // load.
  static constexpr std::int64_t kMaxWarmupLookbackDays = 7500;
  static Timeline loadWithWarmup(BarSource &BarSource,
                                 CalendarSource &CalendarSource,
                                 const std::string &SnapshotId,
                                 std::vector<Instrument> Instruments,
                                 Timeframe Timeframe, std::int32_t FromYmd,
                                 std::int32_t ToYmd, std::int64_t WarmupSteps);

  std::int64_t totalSteps() const {
    return static_cast<std::int64_t>(StepStart.size()) - 1;
  }
  // Бары шага по возрастанию instrument.
  std::span<const StepBar> barsAt(std::int64_t Step) const;
  // nullptr, если у бумаги на шаге нет бара.
  const StepBar *barOf(int Instrument, std::int64_t Step) const;
  // Последний бар бумаги на шагах 0…upToStep; nullptr, если баров ещё не было.
  const StepBar *lastBarOf(int Instrument, std::int64_t UpToStep) const;
  std::int32_t tradingDayOf(std::int64_t Step) const; // 1…N
  std::int32_t yyyymmddOf(std::int64_t Step) const;
  bool isLastStepOfDay(std::int64_t Step) const;
  std::int32_t tradingDays() const { return TradingDays; }
  // Торговые дни fromDay…toDay (1…N, включительно); шаги и дни перенумерованы с
  // 0 и с 1. Прогрев среза - те же W шагов, что у шкалы, взятые непосредственно
  // перед fromDay (из дней шкалы и её прогрева; меньше, если раньше столько
  // шагов нет).
  Timeline slice(std::int32_t FromDay, std::int32_t ToDay) const;
  const std::vector<Instrument> &instruments() const { return Instruments; }

  // История до начала шкалы - прогрев индикаторов (`MarketSpec.warmup_bars`).
  // Шаги прогрева -W…-1, торговые дни
  // <= 0 (последний день истории - 0, день перед ним - -1, ...). Торговли и
  // кривой стоимости на них нет; это прошлое относительно шага 0, поэтому
  // пользователь видит их с самого начала (INV-1). Бары - по возрастанию
  // (step, instrument). Без прогрева - пусто, W = 0.
  std::int64_t warmupSteps() const { return WarmupSteps; }
  std::span<const StepBar> warmupBars() const { return Warmup; }
  // Копия шкалы с прогревом: последние steps шагов history (меньше, если в
  // history меньше шагов). Бумаги history - те же, что у шкалы (ключи по
  // порядку), и все бары history раньше первого бара шкалы - иначе
  // std::invalid_argument. steps < 0 - std::invalid_argument; прежний прогрев
  // заменяется.
  Timeline withWarmup(const Timeline &History, std::int64_t Steps) const;

private:
  Timeline() = default;
  static Timeline buildWithInterval(std::vector<Instrument> Instruments,
                                    const std::vector<std::vector<Bar>> &Bars,
                                    const std::vector<TradingDay> &Calendar,
                                    std::int64_t IntervalMs);
  void checkStep(std::int64_t Step) const;
  void checkInstrument(int Instrument) const;
  void buildIndex();

  std::vector<Instrument> Instruments;
  std::vector<StepBar> Bars; // по возрастанию (step, instrument)
  std::vector<std::size_t> StepStart; // totalSteps + 1 смещений в bars_
  std::vector<std::int32_t> StepDay;  // торговый день шага
  std::vector<std::int32_t> StepYmd;  // московская дата шага
  std::vector<std::vector<std::size_t>>
      ByInstrument; // индексы в bars_ по возрастанию шага
  std::int32_t TradingDays = 0;
  std::vector<StepBar>
      Warmup; // шаги -warmupSteps_…-1, по возрастанию (step, instrument)
  std::int64_t WarmupSteps = 0;
};

} // namespace core
