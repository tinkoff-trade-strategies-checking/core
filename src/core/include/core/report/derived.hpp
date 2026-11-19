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

// Портфель на шаге: рубли и проценты. База ВСЕХ процентов - начальный капитал
// (`decisions.md`, «Слепой режим»), поэтому CashPct + InvestedPct = EquityPct
// (с точностью до 0,01 из-за отдельных округлений).
struct PortfolioPcts {
  Decimal Invested; // сумма стоимости позиций, ₽
  Decimal Pnl;      // equity - начальный капитал, ₽
  Decimal Costs;    // накопленные издержки сделок, ₽
  Decimal EquityPct, CashPct, InvestedPct, PnlPct, CostsPct;
};

// Накопленные издержки сделок (Fill::cost()) до шага включительно: префиксные
// суммы по сделкам в порядке исполнения (шаги не убывают), запрос - O(log n).
class CostLedger {
public:
  explicit CostLedger(const std::vector<Fill> &Fills);
  Decimal upTo(std::int64_t Step) const;

private:
  std::vector<std::int64_t> Steps;
  std::vector<Decimal> Prefix; // Prefix[i] - сумма первых i сделок
};

// InitialCash > 0, иначе std::invalid_argument.
PortfolioPcts portfolioPcts(const PortfolioSnapshot &Snapshot,
                            const Decimal &InitialCash,
                            const Decimal &CumulativeCosts);

// Стоимость позиции в % начального капитала.
Decimal positionValuePct(const Decimal &MarketValue,
                         const Decimal &InitialCash);

// Нереализованный P&L позиции в % начального капитала:
// (MarketValue - CostBasis) / InitialCash.
Decimal positionPnlPct(const Decimal &MarketValue, const Decimal &CostBasis,
                       const Decimal &InitialCash);

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
