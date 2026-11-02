#pragma once

#include <optional>
#include <utility>

#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/report/metrics.hpp"
#include "core/run/backtest.hpp"
#include "handlers.hpp"

// Отчёт: вход метрик из состояния движка или результата прогона и перевод отчёта в proto (ФТ-17). Чистые функции (INV-4).

namespace worker {

// Вход метрик по состоянию движка на текущем шаге: кривая стоимости, доли позиций, пары покупка - продажа, отказы и
// частичные исполнения. initialCash - капитал из SessionSpec (движок его не отдаёт).
core::MetricsInput metricsInput(const core::SessionEngine& engine, const core::Decimal& initialCash,
                                const core::Timeline& timeline,
                                std::optional<std::pair<core::Decimal, core::Decimal>> benchmark);

// То же по результату прогона (RunOutput) и заданному начальному капиталу.
core::MetricsInput metricsInput(const core::RunOutput& output, const core::Decimal& initialCash,
                                const core::Timeline& timeline,
                                std::optional<std::pair<core::Decimal, core::Decimal>> benchmark);

// Метрики, издержки, кривая стоимости и итоговая стоимость; необязательные метрики без значения не задаются.
// Уровень отчёта OK, находок нет: их добавляет вызывающий (прогон - convert/run).
void toProto(const core::Report& report, pb::Report* out);
void toProto(const core::Finding& finding, pb::Finding* out);

}  // namespace worker
