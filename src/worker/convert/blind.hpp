#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "core/blind/blind.hpp"
#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/report/metrics.hpp"
#include "handlers.hpp"

// Конвертер слепого вида сессии ядро -> proto (`decisions.md`, «Слепой режим»).

namespace worker {

// Заполняет BlindSessionView для ответа ReplaySessionResponse.
// Report передаётся только при include_report = true.
void fillBlindView(const core::Timeline &Timeline,
                   const core::Decimal &InitialCash, std::int64_t LastStep,
                   bool IncludeHistory,
                   std::span<const core::StepEvents> Events,
                   const core::SessionEngine &Engine,
                   const std::optional<core::Report> &Report,
                   pb::BlindSessionView *Out);

} // namespace worker
