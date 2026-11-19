#include "convert/blind.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "convert/state.hpp"
#include "core/money/proto_conv.hpp"
#include "core/report/derived.hpp"

namespace worker {

namespace {

void blindBarToProto(const core::BlindBar &Bar, pb::BlindBar *Out) {
  Out->set_step(Bar.Step);
  Out->set_instrument_index(static_cast<std::int32_t>(Bar.Instrument));
  Out->set_trading_day(Bar.TradingDay);
  core::toProto(Bar.Open, Out->mutable_open());
  core::toProto(Bar.High, Out->mutable_high());
  core::toProto(Bar.Low, Out->mutable_low());
  core::toProto(Bar.Close, Out->mutable_close());
  core::toProto(Bar.VolumeRel, Out->mutable_volume_rel());
  Out->set_after_halt(Bar.AfterHalt);
}

void blindPortfolioToProto(const core::BlindPortfolio &Portfolio,
                           pb::BlindPortfolio *Out) {
  core::toProto(Portfolio.CashPct, Out->mutable_cash_pct());
  core::toProto(Portfolio.EquityPct, Out->mutable_equity_pct());
  core::toProto(Portfolio.PnlPct, Out->mutable_pnl_pct());
  core::toProto(Portfolio.InvestedPct, Out->mutable_invested_pct());
  core::toProto(Portfolio.CostsPct, Out->mutable_costs_pct());
  for (const core::BlindPosition &P : Portfolio.Positions) {
    pb::BlindPosition *Pos = Out->add_positions();
    Pos->set_instrument_index(static_cast<std::int32_t>(P.Instrument));
    Pos->set_lots(P.Lots);
    core::toProto(P.AvgPrice, Pos->mutable_avg_price());
    core::toProto(P.MarketValuePct, Pos->mutable_market_value_pct());
  }
}

void blindOrderToProto(const core::BlindOrder &Order, pb::BlindOrder *Out) {
  Out->set_id(Order.Id);
  Out->set_instrument_index(static_cast<std::int32_t>(Order.Instrument));
  if (Order.LimitPrice)
    core::toProto(*Order.LimitPrice, Out->mutable_limit_price());
  if (Order.StopPrice)
    core::toProto(*Order.StopPrice, Out->mutable_stop_price());
  if (Order.TrailingStopLevel) {
    core::toProto(*Order.TrailingStopLevel, Out->mutable_trailing_stop_level());
  }
}

void blindFillToProto(const core::BlindFill &Fill, pb::BlindFill *Out) {
  Out->set_order_id(Fill.OrderId);
  Out->set_step(Fill.Step);
  Out->set_instrument_index(static_cast<std::int32_t>(Fill.Instrument));
  Out->set_side(toProto(Fill.OrderSide));
  Out->set_lots(Fill.Lots);
  core::toProto(Fill.Price, Out->mutable_price());
  core::toProto(Fill.BasePrice, Out->mutable_base_price());
  core::toProto(Fill.ValuePct, Out->mutable_value_pct());
  core::toProto(Fill.CommissionPct, Out->mutable_commission_pct());
  core::toProto(Fill.FeePct, Out->mutable_exchange_fee_pct());
  core::toProto(Fill.SpreadCostPct, Out->mutable_spread_cost_pct());
  core::toProto(Fill.SlippageCostPct, Out->mutable_slippage_cost_pct());
  Out->set_stop_and_take_same_bar(Fill.StopAndTakeSameBar);
  core::toProto(Fill.CostPct, Out->mutable_cost_pct());
  Out->set_seq(Fill.Seq);
}

void blindReportToProto(const core::BlindReport &Report, pb::BlindReport *Out) {
  pb::BlindCostTotals *Costs = Out->mutable_costs();
  core::toProto(Report.Costs.CommissionPct, Costs->mutable_commission_pct());
  core::toProto(Report.Costs.FeePct, Costs->mutable_exchange_fee_pct());
  core::toProto(Report.Costs.SpreadPct, Costs->mutable_spread_pct());
  core::toProto(Report.Costs.SlippagePct, Costs->mutable_slippage_pct());
  core::toProto(Report.Costs.TotalPct, Costs->mutable_total_pct());
  for (const core::UnderwaterPoint &P : Report.Underwater) {
    pb::UnderwaterPoint *Pt = Out->add_underwater();
    Pt->set_step(P.Step);
    core::toProto(P.DrawdownPct, Pt->mutable_drawdown_pct());
  }

  for (const core::BlindEquityPoint &P : Report.Equity) {
    pb::BlindEquityPoint *Pt = Out->add_equity();
    Pt->set_step(P.Step);
    core::toProto(P.EquityPct, Pt->mutable_equity_pct());
  }
  core::toProto(Report.FinalEquityPct, Out->mutable_final_equity_pct());
  core::toProto(Report.PnlPct, Out->mutable_pnl_pct());

  for (const core::BlindEquityPoint &P : Report.BenchmarkEquity) {
    pb::BlindEquityPoint *Pt = Out->add_benchmark_equity();
    Pt->set_step(P.Step);
    core::toProto(P.EquityPct, Pt->mutable_equity_pct());
  }

  std::int64_t TripSeq = 0;
  for (const core::BlindRoundTrip &T : Report.RoundTrips) {
    pb::BlindRoundTrip *Trip = Out->add_round_trips();
    Trip->set_seq(TripSeq++);
    Trip->set_instrument_index(static_cast<std::int32_t>(T.Instrument));
    Trip->set_entry_step(T.EntryStep);
    Trip->set_exit_step(T.ExitStep);
    Trip->set_lots(T.Lots);
    core::toProto(T.EntryPrice, Trip->mutable_entry_price());
    core::toProto(T.ExitPrice, Trip->mutable_exit_price());
    core::toProto(T.GrossPnlPct, Trip->mutable_gross_pnl_pct());
    core::toProto(T.CostsPct, Trip->mutable_costs_pct());
    core::toProto(T.NetPnlPct, Trip->mutable_net_pnl_pct());
  }
}

} // namespace

void fillBlindView(const core::Timeline &Timeline,
                   const core::Decimal &InitialCash, std::int64_t LastStep,
                   bool IncludeHistory,
                   std::span<const core::StepEvents> Events,
                   const core::SessionEngine &Engine,
                   const std::optional<core::Report> &Report,
                   pb::BlindSessionView *Out) {
  const std::vector<core::BlindScale> Scales =
      core::computeBlindScales(Timeline, InitialCash, LastStep);

  for (std::size_t Idx = 0; Idx < Timeline.instruments().size(); ++Idx) {
    pb::BlindInstrument *Inst = Out->add_instruments();
    Inst->set_instrument_index(static_cast<std::int32_t>(Idx));
    if (Scales[Idx].LotValueAtStart) {
      core::toProto(*Scales[Idx].LotValueAtStart,
                    Inst->mutable_lot_value_at_start());
    }
    const core::InstrumentChange Change =
        core::instrumentChange(Timeline, static_cast<int>(Idx), LastStep);
    if (Change.StepPct)
      core::toProto(*Change.StepPct, Inst->mutable_change_step_pct());
    if (Change.PeriodPct)
      core::toProto(*Change.PeriodPct, Inst->mutable_change_period_pct());
  }

  for (const core::StepEvents &E : Events) {
    const core::BlindStepEvents BlindEvents = core::toBlindStepEvents(
        E, InitialCash, Scales, core::cumulativeCosts(Engine.fills(), E.Step));
    pb::BlindStepEvents *Ev = Out->add_events();
    Ev->set_step(BlindEvents.Step);
    for (const core::BlindBar &B : BlindEvents.Bars)
      blindBarToProto(B, Ev->add_bars());
    for (const core::BlindFill &F : BlindEvents.Fills)
      blindFillToProto(F, Ev->add_fills());
    for (const core::BlindOrder &O : BlindEvents.ChangedOrders)
      blindOrderToProto(O, Ev->add_changed_orders());
    blindPortfolioToProto(BlindEvents.Portfolio, Ev->mutable_portfolio());
  }

  if (IncludeHistory) {
    // До появления P0 (первого бара <= LastStep) бумага отсутствует в слепом
    // виде целиком — её бары прогрева тоже не отдаются.
    for (const core::StepBar &Bar : Timeline.warmupBars()) {
      const core::BlindScale &Scale =
          Scales.at(static_cast<std::size_t>(Bar.Instrument));
      if (!Scale.P0)
        continue;
      blindBarToProto(core::toBlindBar(Bar, Scale), Out->add_history());
    }
    for (std::int64_t Step = 0; Step <= LastStep; ++Step) {
      for (const core::StepBar &Bar : Timeline.barsAt(Step)) {
        const core::BlindScale &Scale =
            Scales.at(static_cast<std::size_t>(Bar.Instrument));
        blindBarToProto(core::toBlindBar(Bar, Scale), Out->add_history());
      }
    }
  }

  blindPortfolioToProto(
      core::toBlindPortfolio(Engine.snapshot(), InitialCash, Scales,
                             core::cumulativeCosts(Engine.fills(), LastStep)),
      Out->mutable_portfolio());

  for (const core::Order &O : Engine.orders()) {
    if (O.Spec.Instrument < 0 ||
        static_cast<std::size_t>(O.Spec.Instrument) >= Scales.size()) {
      continue;
    }
    const core::BlindScale &Scale =
        Scales[static_cast<std::size_t>(O.Spec.Instrument)];
    if (!Scale.P0)
      continue;
    blindOrderToProto(core::toBlindOrder(O, Scale), Out->add_orders());
  }

  for (const core::Fill &F : Engine.fills()) {
    blindFillToProto(
        core::toBlindFill(F, InitialCash,
                          Scales.at(static_cast<std::size_t>(F.Instrument))),
        Out->add_fills());
  }

  if (Report) {
    blindReportToProto(core::toBlindReport(*Report, InitialCash, Scales),
                       Out->mutable_report());
  }
}

} // namespace worker
