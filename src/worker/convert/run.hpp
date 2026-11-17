#pragma once

#include <optional>

#include "core/market/timeline.hpp"
#include "core/report/metrics.hpp"
#include "core/run/backtest.hpp"
#include "handlers.hpp"

// Результат прогона -> pb::RunResult. Единственный перевод результата прогона:
// RunBacktest (задача 59), walk-forward (28) и разбор расхождений (29) только
// вызывают его. Чистая функция (INV-4).

namespace worker {

// status, level, отчёт (метрики, издержки, кривая, находки прогона), сделки,
// заявки, digest, last_step, total_steps = число шагов шкалы. cost_breakdown
// задаётся только при наличии цепочки издержек (ФТ-24): шаги - в том порядке, в
// котором их отдало ядро.
void toProto(const core::RunOutput &Output, const core::Report &Report,
             const std::optional<core::CostBreakdown> &CostBreakdown,
             const core::Timeline &Timeline, pb::RunResult *Out);

pb::RunStatus toProto(core::RunStatus Status);
pb::CostFactor toProto(core::CostFactor Factor);
pb::CostFlag toProto(core::CostFlag Flag);

} // namespace worker
