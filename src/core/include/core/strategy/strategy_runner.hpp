#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/engine/journal.hpp"
#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/report/benchmark.hpp"
#include "core/report/metrics.hpp"
#include "core/report/ruonia.hpp"
#include "dsl/types.hpp"

// Запуск DSL-стратегии поверх `SessionEngine` (прогрев, торговые шаги,
// управление заявками правил, кооперативная отмена и проверки ФТ-14).

namespace core {

struct StrategyRunOptions {
  std::string Source;
  std::map<std::string, double> ParamOverrides;
  std::function<bool()> ShouldCancel;
  std::optional<std::chrono::steady_clock::duration> TimeBudget;
  std::optional<std::int64_t> UpToStep;
  std::function<void(std::int64_t DoneSteps, std::int64_t TotalSteps)>
      OnProgress;
};

struct StrategyRunResult {
  bool Compiled = false;
  bool Cancelled = false;
  bool Invalid = false;
  std::vector<dsl::Diagnostic> Diagnostics;
  CheckLevel StaticLookahead = CheckLevel::Ok;
  std::vector<Finding> RuntimeFindings;
  std::vector<JournalEntry> Journal;
  std::vector<CommandResult> CommandResults;
  std::optional<SessionEngine> Engine;
  CheckLevel FinalLevel = CheckLevel::Ok;
};

// Компилирует и прогоняет стратегию на шкале `Tl` с параметрами `Spec`.
StrategyRunResult runStrategy(const SessionSpec &Spec,
                              std::shared_ptr<const Timeline> Tl,
                              const StrategyRunOptions &Opts);

// Формирует отчёт по результатам `RunResult` с применением проверок ФТ-14
// (`StaticLookahead`, `E-RT01`, `W-RT01`, `W-RT02`).
Report buildStrategyReport(
    const StrategyRunResult &RunResult, const Decimal &InitialCash,
    const Timeline &Tl,
    const std::optional<BenchmarkSeries> &Benchmark = std::nullopt,
    const std::optional<RuoniaSeries> &Ruonia = std::nullopt,
    std::int32_t RuoniaPreviousDayYmd = 0);

} // namespace core
