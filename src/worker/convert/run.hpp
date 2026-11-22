#pragma once

#include <map>
#include <string>

#include "core/market/timeline.hpp"
#include "core/report/metrics.hpp"
#include "core/strategy/strategy_runner.hpp"
#include "dsl/types.hpp"
#include "handlers.hpp"

// Конвертеры DSL-стратегий и результатов прогона: param_overrides -> map,
// диагностики DSL и StrategyRunResult -> proto. Чистые функции (INV-4).

namespace worker {

// Разбирает param_overrides; дублирующиеся имена -> std::invalid_argument.
std::map<std::string, double> parseParamOverrides(
    const google::protobuf::RepeatedPtrField<pb::ParamValue> &Overrides);

pb::Level toProto(dsl::LookaheadLevel Lookahead);
pb::Level toProto(dsl::DiagnosticSeverity Severity);
void toProto(const dsl::Diagnostic &Diag, pb::Diagnostic *Out);

// Заполняет RunResult: при !Run.Compiled || Run.Invalid выставляет INVALID и
// ERROR; при наличии Engine переносит отчёт (если передан), сделки, заявки,
// журнал, digest и (при IncludeHistory) бары прогрева + [0; last_step].
void toProto(const core::StrategyRunResult &Run, const core::Report *ReportOpt,
             const core::Timeline &Timeline, bool IncludeHistory,
             pb::RunResult *Out);

} // namespace worker
