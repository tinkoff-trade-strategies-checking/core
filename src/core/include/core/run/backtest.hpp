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
#include "core/report/metrics.hpp"  // Finding

// Прогон стратегии по данным (ФТ-1, ФТ-8, ФТ-14, INV-1, INV-3, INV-4).
// Стратегия идёт тем же движком `SessionEngine`, что и журнал человека: намерения превращаются в записи журнала
// и применяются через `SessionEngine::apply` (INV-3). Стратегия видит бары только до курсора (INV-1).
// Книга заявок: на правило и бумагу - не больше одной активной заявки; продажи стратегии по бумаге с нулевой позицией
// снимаются сами (`PositionClosed`).

namespace core {

enum class RunStatus {
  Completed,     // дошёл до конца шкалы времени
  BankruptStop,  // остановлен: стоимость портфеля <= 0
  Invalid        // нарушена целостность движка (E-RT01): прогон остановлен и не засчитывается
};

struct RunOutput {
  RunStatus status = RunStatus::Completed;
  dsl::Level level = dsl::Level::Ok;  // худший из уровня версии и находок прогона
  std::int64_t lastStep = 0;
  std::vector<Fill> fills;
  std::vector<Order> orders;
  std::vector<EquityPoint> equity;          // по точке на шаг, с 0
  std::vector<RoundTrip> roundTrips;
  std::vector<Finding> findings;            // E-RT01, W-RT01, W-RT02
  std::vector<JournalEntry> journal;        // записи стратегии: replaySession с ними даёт те же сделки (INV-3)
  std::vector<std::vector<Decimal>> positionValuesByStep;  // [шаг][бумага], в метрики (HHI)
  std::uint64_t digest = 0;
  Decimal finalEquity;
  // Отказы: заявки со статусом Rejected (отказ при подаче). Частичные исполнения: заявки, у которых исполнено
  // больше нуля лотов, но меньше заказанного (в том числе остаток снят или заявка ещё активна).
  std::int64_t rejections = 0, partialFills = 0;
};

// Прогресс: сколько шагов пройдено из скольких. Вызывается каждые 1000 шагов и в конце; может быть пустым.
using Progress = std::function<void(std::int64_t done, std::int64_t total)>;

// params - результат check() программы, overrides - значения параметров на прогон (INV-1: StrategyInstance сам
// заново проверяет программу и отказывает при ошибке - std::invalid_argument, он не превращается в Invalid).
// staticLevel - уровень версии по статической проверке. Бумаги spec должны совпадать с бумагами шкалы времени
// (иначе std::invalid_argument из SessionEngine).
RunOutput runBacktest(const SessionSpec& spec, std::shared_ptr<const Timeline> timeline, const dsl::Program& program,
                      const std::vector<dsl::ResolvedParam>& params, const dsl::ParamValues& overrides,
                      dsl::Level staticLevel, const Progress& progress);

// FNV-1a 64 по каноническому тексту прогона: сделки, точки кривой стоимости, статус (INV-4).
std::uint64_t digestOf(const RunOutput& output);

namespace detail {

// Для тестов: вызывается на каждом шаге после обработки шага движком, до решений стратегии. Исключение
// IntegrityViolation оттуда обрабатывается как нарушение целостности самого движка (E-RT01).
using StepHook = std::function<void(const SessionEngine&)>;

RunOutput runBacktestWithHook(const SessionSpec& spec, std::shared_ptr<const Timeline> timeline,
                              const dsl::Program& program, const std::vector<dsl::ResolvedParam>& params,
                              const dsl::ParamValues& overrides, dsl::Level staticLevel, const Progress& progress,
                              const StepHook& hook);

// Прогон по шагам 0…lastStep (разбор расхождений по сессии, остановленной раньше конца шкалы): на lastStep стратегия
// ещё решает, но заявки не исполняются, бары позже lastStep не используются (INV-1). lastStep вне шкалы -
// std::invalid_argument. Остальное - как у runBacktest.
RunOutput runBacktestToStep(const SessionSpec& spec, std::shared_ptr<const Timeline> timeline,
                            const dsl::Program& program, const std::vector<dsl::ResolvedParam>& params,
                            const dsl::ParamValues& overrides, dsl::Level staticLevel, std::int64_t lastStep);

// Проверки «странных действий» по итогам прогона (dsl.md, «Чтение будущего»): W-RT01, W-RT02.
// orders - все заявки прогона по возрастанию id (индекс не используется, поиск по id).
std::vector<Finding> runtimeFindings(const Timeline& timeline, const std::vector<Order>& orders,
                                     const std::vector<Fill>& fills, const std::vector<RoundTrip>& roundTrips);

// Находка о нарушении целостности (E-RT01).
Finding integrityFinding(const IntegrityViolation& violation, std::int64_t step);

// Худший уровень: staticLevel и находки.
dsl::Level worstLevel(dsl::Level staticLevel, const std::vector<Finding>& findings);

}  // namespace detail

}  // namespace core
