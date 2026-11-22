#pragma once

#include <optional>
#include <utility>

#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/report/benchmark.hpp"
#include "core/report/metrics.hpp"
#include "core/report/ruonia.hpp"
#include "handlers.hpp"

// Отчёт: вход метрик из состояния движка и перевод
// отчёта в proto (ФТ-17). Чистые функции (INV-4).

namespace worker {

// Вход метрик по состоянию движка на текущем шаге: кривая стоимости, доли
// позиций, пары покупка - продажа, отказы и частичные исполнения. initialCash -
// капитал из SessionSpec (движок его не отдаёт). Бенчмарк - доходность за
// период и кривая по полностью пройденным дням шагов 0…последний шаг кривой
// стоимости. RUONIA - ставки дней для Sharpe и Sortino; RuoniaPreviousDayYmd -
// торговый день перед первым (0 - нет, Delta первого дня = 1).
core::MetricsInput
metricsInput(const core::SessionEngine &Engine,
             const core::Decimal &InitialCash, const core::Timeline &Timeline,
             const std::optional<core::BenchmarkSeries> &Benchmark,
             const std::optional<core::RuoniaSeries> &Ruonia = std::nullopt,
             std::int32_t RuoniaPreviousDayYmd = 0);

pb::Level toProto(core::CheckLevel Severity);
// Обратно к toProto(CheckLevel); неизвестное значение - std::invalid_argument.
core::CheckLevel checkLevelFromProto(pb::Level Value);
void toProto(const core::Finding &Item, pb::Finding *Out);

// Метрики, издержки, кривая стоимости, итоговая стоимость и диагностики ФТ-14;
// необязательные метрики без значения не задаются. Пары FIFO получают ключ
// бумаги из шкалы времени.
void toProto(const core::Report &Report, const core::Timeline &Timeline,
             pb::Report *Out);

} // namespace worker
