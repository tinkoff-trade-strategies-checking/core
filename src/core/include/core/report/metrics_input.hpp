#pragma once

#include <cstdint>
#include <optional>

#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/report/benchmark.hpp"
#include "core/report/metrics.hpp"
#include "core/report/ruonia.hpp"

// Сборка входа метрик отчёта (MetricsInput) из состояния движка и шкалы
// времени. Единая точка для simulation_worker и sim_cli (INV-1, INV-4).

namespace core {

// Последний полностью пройденный торговый день (yyyymmdd) на шагах
// 0…LastStep (INV-1: бар D1 незавершённого дня содержит будущее).
// Если ни одного полного дня нет — nullopt.
std::optional<std::int32_t> lastCompletedDayYmd(const Timeline &Timeline,
                                                std::int64_t LastStep);

// Вход метрик по состоянию движка на текущем шаге: кривая стоимости, доли
// позиций, пары покупка -> продажа, отказы и частичные исполнения, бенчмарк
// (доходность и кривая по полностью пройденным дням) и сопоставленные ставки
// RUONIA. RuoniaPreviousDayYmd — торговый день перед первым (0 — нет, Delta
// первого дня = 1).
MetricsInput buildMetricsInput(
    const SessionEngine &Engine, const Decimal &InitialCash,
    const Timeline &Timeline,
    const std::optional<BenchmarkSeries> &Benchmark = std::nullopt,
    const std::optional<RuoniaSeries> &Ruonia = std::nullopt,
    std::int32_t RuoniaPreviousDayYmd = 0);

} // namespace core
