#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "core/dsl/ast.hpp"
#include "core/dsl/checker.hpp"
#include "core/dsl/diagnostic.hpp"
#include "core/dsl/interpreter.hpp"
#include "core/engine/session_engine.hpp"
#include "core/exec/order.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp"
#include "core/report/metrics.hpp" // Finding

// Прогон стратегии по данным (ФТ-1, ФТ-8, ФТ-14, INV-1, INV-3, INV-4).
// Стратегия идёт тем же движком `SessionEngine`, что и журнал человека:
// намерения превращаются в записи журнала и применяются через
// `SessionEngine::apply` (INV-3). Стратегия видит бары только до курсора
// (INV-1). Книга заявок: на правило и бумагу - не больше одной активной заявки;
// продажи стратегии по бумаге с нулевой позицией снимаются сами
// (`PositionClosed`).

namespace core {

enum class RunStatus {
  Completed,    // дошёл до конца шкалы времени
  BankruptStop, // остановлен: стоимость портфеля <= 0
  Invalid // нарушена целостность движка (E-RT01): прогон остановлен и не
          // засчитывается
};

struct RunOutput {
  RunStatus Status = RunStatus::Completed;
  dsl::Level Severity =
      dsl::Level::Ok; // худший из уровня версии и находок прогона
  std::int64_t LastStep = 0;
  std::vector<Fill> Fills;
  std::vector<Order> Orders;
  std::vector<EquityPoint> Equity; // по точке на шаг, с 0
  std::vector<RoundTrip> RoundTrips;
  std::vector<Finding> Findings; // E-RT01, W-RT01, W-RT02
  std::vector<JournalEntry> Journal; // записи стратегии: replaySession с ними
                                     // даёт те же сделки (INV-3)
  std::vector<std::vector<Decimal>>
      PositionValuesByStep; // [шаг][бумага], в метрики (HHI)
  std::uint64_t Digest = 0;
  Decimal FinalEquity;
  // Отказы: заявки со статусом Rejected (отказ при подаче). Частичные
  // исполнения: заявки, у которых исполнено больше нуля лотов, но меньше
  // заказанного (в том числе остаток снят или заявка ещё активна).
  std::int64_t Rejections = 0, PartialFills = 0;
};

// Прогресс: сколько шагов пройдено из скольких. Вызывается каждые 1000 шагов и
// в конце; может быть пустым.
using Progress = std::function<void(std::int64_t Done, std::int64_t Total)>;

// params - результат check() программы, overrides - значения параметров на
// прогон (INV-1: StrategyInstance сам заново проверяет программу и отказывает
// при ошибке - std::invalid_argument, он не превращается в Invalid).
// staticLevel - уровень версии по статической проверке. Бумаги spec должны
// совпадать с бумагами шкалы времени (иначе std::invalid_argument из
// SessionEngine).
RunOutput runBacktest(const SessionSpec &Spec,
                      std::shared_ptr<const Timeline> Timeline,
                      const dsl::Program &Program,
                      const std::vector<dsl::ResolvedParam> &Params,
                      const dsl::ParamValues &Overrides, dsl::Level StaticLevel,
                      const Progress &Progress);

// FNV-1a 64 по каноническому тексту прогона: сделки, точки кривой стоимости,
// статус (INV-4).
std::uint64_t digestOf(const RunOutput &Output);

namespace detail {

// Для тестов: вызывается на каждом шаге после обработки шага движком, до
// решений стратегии. Исключение IntegrityViolation оттуда обрабатывается как
// нарушение целостности самого движка (E-RT01).
using StepHook = std::function<void(const SessionEngine &)>;

RunOutput runBacktestWithHook(const SessionSpec &Spec,
                              std::shared_ptr<const Timeline> Timeline,
                              const dsl::Program &Program,
                              const std::vector<dsl::ResolvedParam> &Params,
                              const dsl::ParamValues &Overrides,
                              dsl::Level StaticLevel, const Progress &Progress,
                              const StepHook &Hook);

// Прогон по шагам 0…lastStep (разбор расхождений по сессии, остановленной
// раньше конца шкалы): на lastStep стратегия ещё решает, но заявки не
// исполняются, бары позже lastStep не используются (INV-1). lastStep вне шкалы
// - std::invalid_argument. Остальное - как у runBacktest.
RunOutput runBacktestToStep(const SessionSpec &Spec,
                            std::shared_ptr<const Timeline> Timeline,
                            const dsl::Program &Program,
                            const std::vector<dsl::ResolvedParam> &Params,
                            const dsl::ParamValues &Overrides,
                            dsl::Level StaticLevel, std::int64_t LastStep);

// Проверки «странных действий» по итогам прогона (dsl.md, «Чтение будущего»):
// W-RT01, W-RT02. orders - все заявки прогона по возрастанию id (индекс не
// используется, поиск по id).
std::vector<Finding> runtimeFindings(const Timeline &Timeline,
                                     const std::vector<Order> &Orders,
                                     const std::vector<Fill> &Fills,
                                     const std::vector<RoundTrip> &RoundTrips);

// Прогрев стратегии на барах истории до шага 0 (`Timeline::warmupBars`): ряды и
// индикаторы обновляются, правила вычисляются с пустым портфелем (деньги =
// стоимость = начальный капитал), намерения отбрасываются - торговли до шага 0
// нет. Вызывается один раз, до первого бара шкалы.
void warmUp(dsl::StrategyInstance &Strategy, const Timeline &Timeline,
            const SessionSpec &Spec);
// Находка о нарушении целостности (E-RT01).
Finding integrityFinding(const IntegrityViolation &Violation,
                         std::int64_t Step);

// Худший уровень: staticLevel и находки.
dsl::Level worstLevel(dsl::Level StaticLevel,
                      const std::vector<Finding> &Findings);

} // namespace detail

} // namespace core
