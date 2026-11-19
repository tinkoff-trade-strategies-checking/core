#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "core/engine/session_engine.hpp" // PortfolioSnapshot
#include "core/exec/order.hpp"            // Fill
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"

// Производные величины для экранов (core#54, `decisions.md`, «Кто считает»).
// Все проценты - уже ×100, 2 знака HALF_UP (`decisions.md`, «Деньги и
// округление»), считаются от Decimal без double (INV-6).

namespace core {

// Портфель в процентах на шаге.
struct PortfolioPcts {
  Decimal InvestedPct; // стоимость позиций / equity; 0 при equity <= 0
  Decimal CostsPct; // накопленные издержки сделок / начальный капитал
  Decimal PnlPct; // (equity - начальный капитал) / начальный капитал
};

// Сумма издержек сделок (Fill::cost()) на шагах <= UpToStep.
Decimal cumulativeCosts(const std::vector<Fill> &Fills, std::int64_t UpToStep);

// InitialCash > 0, иначе std::invalid_argument.
PortfolioPcts portfolioPcts(const PortfolioSnapshot &Snapshot,
                            const Decimal &InitialCash,
                            const Decimal &CumulativeCosts);

// Доля позиции в equity: MarketValue / Equity; 0 при equity <= 0.
Decimal positionSharePct(const Decimal &MarketValue, const Decimal &Equity);

// Нереализованный P&L позиции: (MarketValue - Avg*Lots*LotSize) /
// (Avg*Lots*LotSize); 0 при нулевой базе.
Decimal positionPnlPct(const Decimal &MarketValue, const Decimal &AvgPrice,
                       std::int64_t Lots, std::int64_t LotSize);

// Изменение цены бумаги на курсоре (INV-1: только бары шагов <= LastStep).
// StepPct: close последнего бара <= LastStep к close предыдущего бара
// (шаги 0…, затем прогрев; нет предыдущего - к open этого же бара).
// PeriodPct: close последнего бара к open первого бара сессии (шаг >= 0).
// Нет баров на шагах 0…LastStep - оба nullopt.
struct InstrumentChange {
  std::optional<Decimal> StepPct, PeriodPct;
};
InstrumentChange instrumentChange(const Timeline &Timeline, int Instrument,
                                  std::int64_t LastStep);

} // namespace core
