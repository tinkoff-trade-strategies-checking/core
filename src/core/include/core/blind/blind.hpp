#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/engine/session_engine.hpp"
#include "core/exec/order.hpp"
#include "core/market/step_bar.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp"
#include "core/report/metrics.hpp"

// Расчёты слепого режима (`decisions.md`, «Слепой режим», INV-1, INV-6):
// - цены баров и заявок/сделок: x * 100 / P0 (P0 = open шага 0 по бумаге,
//   бары прогрева тем же P0), 2 знака HALF_UP;
// - объём: V_rel = 100 * V / V_ref (средний объём баров прогрева, без прогрева
//   — объём бара шага 0, при 0 — 1 лот), 2 знака HALF_UP;
// - деньги, стоимость позиций, P&L, суммы сделок, издержки и lotValueAtStart:
//   в % от начального капитала, 2 знака HALF_UP.

namespace core {

struct BlindScale {
  std::optional<Decimal> P0;
  std::int64_t VolSum = 1;
  std::int64_t VolCount = 1;
  std::optional<Decimal> LotValueAtStart;
};

struct BlindBar {
  std::int64_t Step = 0;
  int Instrument = 0;
  std::int64_t TimeUnixMs = 0;
  std::int32_t TradingDay = 0;
  Decimal Open;
  Decimal High;
  Decimal Low;
  Decimal Close;
  Decimal VolumeRel;
  bool AfterHalt = false;
};

struct BlindPosition {
  int Instrument = 0;
  std::int64_t Lots = 0;
  Decimal AvgPrice;
  Decimal MarketValuePct;
};

struct BlindPortfolio {
  Decimal CashPct;
  Decimal EquityPct;
  Decimal PnlPct;
  std::vector<BlindPosition> Positions;
};

struct BlindOrder {
  std::int64_t Id = 0;
  int Instrument = 0;
  std::optional<Decimal> LimitPrice;
  std::optional<Decimal> StopPrice;
  std::optional<Decimal> TrailingStopLevel;
};

struct BlindFill {
  std::int64_t OrderId = 0;
  std::int64_t Step = 0;
  int Instrument = 0;
  Side OrderSide = Side::Buy;
  std::int64_t Lots = 0;
  Decimal Price;
  Decimal BasePrice;
  Decimal ValuePct;
  Decimal CommissionPct;
  Decimal FeePct;
  Decimal SpreadCostPct;
  Decimal SlippageCostPct;
  bool StopAndTakeSameBar = false;
};

struct BlindStepEvents {
  std::int64_t Step = 0;
  std::vector<BlindBar> Bars;
  std::vector<BlindFill> Fills;
  std::vector<BlindOrder> ChangedOrders;
  BlindPortfolio Portfolio;
};

struct BlindCostTotals {
  Decimal CommissionPct;
  Decimal FeePct;
  Decimal SpreadPct;
  Decimal SlippagePct;
};

struct BlindEquityPoint {
  std::int64_t Step = 0;
  Decimal EquityPct;
};

struct BlindRoundTrip {
  int Instrument = 0;
  std::int64_t EntryStep = 0;
  std::int64_t ExitStep = 0;
  std::int64_t Lots = 0;
  Decimal EntryPrice;
  Decimal ExitPrice;
  Decimal GrossPnlPct;
  Decimal CostsPct;
  Decimal NetPnlPct;
};

struct BlindReport {
  BlindCostTotals Costs;
  std::vector<BlindEquityPoint> Equity;
  Decimal FinalEquityPct;
  Decimal PnlPct;
  std::vector<BlindEquityPoint> BenchmarkEquity;
  std::vector<BlindRoundTrip> RoundTrips;
};

// Вычисляет масштабы слепого режима для каждой бумаги по шкале времени и
// шагам не позже LastStep (INV-1: будущее после LastStep не используется).
std::vector<BlindScale> computeBlindScales(const Timeline &Timeline,
                                           const Decimal &InitialCash,
                                           std::int64_t LastStep);

// Price * 100 / P0, 2 знака HALF_UP.
Decimal scaleBlindPrice(const Decimal &Price, const Decimal &P0);

// 100 * VolumeLots * VolCount / VolSum, 2 знака HALF_UP.
Decimal scaleBlindVolume(std::int64_t VolumeLots, const BlindScale &Scale);

// Amount * 100 / InitialCash, 2 знака HALF_UP.
Decimal toPercentOfCapital(const Decimal &Amount, const Decimal &InitialCash);

BlindBar toBlindBar(const StepBar &Bar, const BlindScale &Scale);

BlindPortfolio toBlindPortfolio(const PortfolioSnapshot &Snapshot,
                                const Decimal &InitialCash,
                                const std::vector<BlindScale> &Scales);

BlindOrder toBlindOrder(const Order &Order, const BlindScale &Scale);

BlindFill toBlindFill(const Fill &Fill, const Decimal &InitialCash,
                      const BlindScale &Scale);

BlindStepEvents toBlindStepEvents(const StepEvents &Events,
                                  const Decimal &InitialCash,
                                  const std::vector<BlindScale> &Scales);

BlindReport toBlindReport(const Report &Report, const Decimal &InitialCash,
                          const std::vector<BlindScale> &Scales);

} // namespace core
