#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "core/exec/costs.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp" // EquityPoint
#include "core/report/metrics.hpp"
#include "core/report/ruonia.hpp"

// Понятные новичку бенчмарки отчёта (core#81): «Просто купил и держал» (Buy &
// Hold) и «Надёжный вклад» (капитал под ставку RUONIA). Деньги - Decimal
// (INV-6), без часов и случайности (INV-4), только бары шагов 0…LastStep
// (INV-1). Точки кривых - на последнем шаге каждого полностью пройденного
// торгового дня, как у кривой индекса (benchmarkEquity).

namespace core {

// Buy & Hold: капитал делится поровну между бумагами сессии (до копейки
// вниз); каждая бумага покупается рыночной заявкой по open своего первого бара
// на шагах 0…LastStep по тем же правилам цены, комиссии и сбора, что и в
// движке (fillPrice, amounts); неизрасходованный остаток и доля бумаги до её
// первого бара остаются деньгами. Позиция удерживается до LastStep (выход
// издержек не несёт). Лимит ликвидности не применяется. Пусто, если нет бумаг
// или шаг цены бумаги не положителен. InitialCash <= 0 или LastStep вне шкалы -
// std::invalid_argument.
std::vector<EquityPoint> buyHoldEquity(const Timeline &Timeline,
                                       std::int64_t LastStep,
                                       const Decimal &InitialCash,
                                       const CostConfig &Costs);

// «Надёжный вклад»: капитал растёт сложным процентом по ставке RUONIA_{d-1}
// (% годовых, Actual/365, как в Sharpe): E_d = E_{d-1} * (1 + Rate / 100 *
// Delta_d / 365), 9 знаков HALF_UP на шаге, в точке - до копейки HALF_UP.
// Нет ставок до первого дня - MarketDataError.
std::vector<EquityPoint> ruoniaEquity(const RuoniaSeries &Series,
                                      const Timeline &Timeline,
                                      std::int64_t LastStep,
                                      const Decimal &InitialCash,
                                      std::int32_t PreviousDayYmd);

// Доходность по последней точке кривой в процентах (уже x100), 2 знака HALF_UP:
// (equity / initialCash - 1) * 100. Пустая кривая - nullopt.
std::optional<Decimal> returnPct(const std::vector<EquityPoint> &Curve,
                                 const Decimal &InitialCash);

struct SimpleBenchmarks {
  std::vector<EquityPoint> BuyHoldEquity;
  std::vector<EquityPoint> RuoniaEquity;
  std::optional<Decimal> BuyHoldReturnPct;
  std::optional<Decimal> RuoniaReturnPct;

  friend bool operator==(const SimpleBenchmarks &,
                         const SimpleBenchmarks &) = default;
};

// Оба бенчмарка за шаги 0…LastStep. Без ряда RUONIA (nullopt, пустой или без
// ставок до первого дня) кривая вклада пуста, отчёт не падает.
SimpleBenchmarks
buildSimpleBenchmarks(const Timeline &Timeline, std::int64_t LastStep,
                      const Decimal &InitialCash, const CostConfig &Costs,
                      const std::optional<RuoniaSeries> &Ruonia,
                      std::int32_t PreviousDayYmd);

// Переносит бенчмарки в отчёт.
void applySimpleBenchmarks(Report &Report, SimpleBenchmarks Benchmarks);

} // namespace core
