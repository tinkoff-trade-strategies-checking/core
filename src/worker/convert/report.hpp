#pragma once

#include <optional>
#include <utility>

#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/report/benchmark.hpp"
#include "core/report/metrics.hpp"
#include "core/run/backtest.hpp"
#include "handlers.hpp"

// Отчёт: вход метрик из состояния движка или результата прогона и перевод
// отчёта в proto (ФТ-17). Чистые функции (INV-4).

namespace worker {

// Вход метрик по состоянию движка на текущем шаге: кривая стоимости, доли
// позиций, пары покупка - продажа, отказы и частичные исполнения. initialCash -
// капитал из SessionSpec (движок его не отдаёт). Бенчмарк - доходность за
// период и кривая по полностью пройденным дням шагов 0…последний шаг кривой
// стоимости.
core::MetricsInput
metricsInput(const core::SessionEngine &Engine,
             const core::Decimal &InitialCash, const core::Timeline &Timeline,
             const std::optional<core::BenchmarkSeries> &Benchmark);

// То же по результату прогона (RunOutput) и заданному начальному капиталу.
core::MetricsInput
metricsInput(const core::RunOutput &Output, const core::Decimal &InitialCash,
             const core::Timeline &Timeline,
             const std::optional<core::BenchmarkSeries> &Benchmark);

// Метрики, издержки, кривая стоимости и итоговая стоимость; необязательные
// метрики без значения не задаются. Уровень отчёта OK, находок нет: их
// добавляет вызывающий (прогон - convert/run).
void toProto(const core::Report &Report, pb::Report *Out);
void toProto(const core::Finding &Finding, pb::Finding *Out);

} // namespace worker
