#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp" // EquityPoint

// Бенчмарк отчёта (ФТ-17): дневные бары индекса за период и кривая «капитал в
// индексе» по шкале времени сессии. Деньги - Decimal (INV-6), без часов и
// случайности (INV-4).

namespace core {

struct DailyClose {
  std::int32_t yyyymmdd = 0; // московская дата
  Decimal close;
};

// open первого дневного бара периода и close каждого дня по возрастанию даты
// (не пусто).
struct BenchmarkSeries {
  Decimal firstOpen;
  std::vector<DailyClose> closes;
};

// (open первого бара, close последнего) - вход MetricsInput::benchmark. Пустой
// ряд - nullopt.
std::optional<std::pair<Decimal, Decimal>>
benchmarkRange(const BenchmarkSeries &series);

// Кривая бенчмарка в рублях капитала: initialCash * close дня / firstOpen (9
// знаков HALF_UP) на последнем шаге каждого торгового дня шкалы из шагов
// 0…lastStep. День, который пройден не полностью (lastStep - не последний шаг
// дня), и день без бара индекса пропускаются: бар D1 недопройденного дня
// содержит будущее (INV-1). firstOpen <= 0 или lastStep вне шкалы -
// std::invalid_argument.
std::vector<EquityPoint> benchmarkEquity(const BenchmarkSeries &series,
                                         const Decimal &initialCash,
                                         const Timeline &timeline,
                                         std::int64_t lastStep);

} // namespace core
