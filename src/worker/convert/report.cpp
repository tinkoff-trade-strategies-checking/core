#include "convert/report.hpp"

#include <stdexcept>
#include <string>

#include "core/money/proto_conv.hpp"
#include "core/report/metrics_input.hpp"

namespace worker {

namespace {

void metricsToProto(const core::Metrics &M, pb::Metrics *Out) {
  Out->set_total_return(M.TotalReturn);
  Out->set_gross_return(M.GrossReturn);
  if (M.Cagr)
    Out->set_cagr(*M.Cagr);
  Out->set_max_drawdown(M.MaxDrawdown);
  Out->set_max_drawdown_steps(M.MaxDrawdownSteps);
  Out->set_max_drawdown_days(M.MaxDrawdownDays);
  if (M.Volatility)
    Out->set_volatility(*M.Volatility);
  if (M.Sharpe)
    Out->set_sharpe(*M.Sharpe);
  if (M.Sortino)
    Out->set_sortino(*M.Sortino);
  Out->set_turnover(M.Turnover);
  Out->set_fills(M.Fills);
  Out->set_round_trips(M.RoundTrips);
  if (M.WinRate)
    Out->set_win_rate(*M.WinRate);
  Out->set_rejections(M.Rejections);
  Out->set_partial_fills(M.PartialFills);
  Out->set_hhi_avg(M.HhiAvg);
  Out->set_max_position_share(M.MaxPositionShare);
  if (M.BenchmarkReturn)
    Out->set_benchmark_return(*M.BenchmarkReturn);
  Out->set_trading_days(M.TradingDays);
  Out->set_stop_take_same_bar(M.StopTakeSameBar);
}

} // namespace

core::MetricsInput
metricsInput(const core::SessionEngine &Engine,
             const core::Decimal &InitialCash, const core::Timeline &Timeline,
             const std::optional<core::BenchmarkSeries> &Benchmark,
             const std::optional<core::RuoniaSeries> &Ruonia,
             std::int32_t RuoniaPreviousDayYmd) {
  return core::buildMetricsInput(Engine, InitialCash, Timeline, Benchmark,
                                 Ruonia, RuoniaPreviousDayYmd);
}

pb::Level toProto(core::CheckLevel Severity) {
  switch (Severity) {
  case core::CheckLevel::Ok:
    return pb::OK;
  case core::CheckLevel::Warning:
    return pb::WARNING;
  case core::CheckLevel::Error:
    return pb::ERROR;
  }
  return pb::LEVEL_UNSPECIFIED;
}

core::CheckLevel checkLevelFromProto(pb::Level Value) {
  switch (Value) {
  case pb::OK:
    return core::CheckLevel::Ok;
  case pb::WARNING:
    return core::CheckLevel::Warning;
  case pb::ERROR:
    return core::CheckLevel::Error;
  default:
    throw std::invalid_argument("неизвестный уровень проверки: " +
                                std::to_string(static_cast<int>(Value)));
  }
}

void toProto(const core::Finding &Item, pb::Finding *Out) {
  Out->set_code(Item.Code);
  Out->set_level(toProto(Item.Severity));
  Out->set_message(Item.Message);
  Out->set_step(Item.Step);
}

void toProto(const core::Report &Report, const core::Timeline &Timeline,
             pb::Report *Out) {
  metricsToProto(Report.Stats, Out->mutable_metrics());
  pb::CostTotals *Costs = Out->mutable_costs();
  core::toProto(Report.Costs.Commission, Costs->mutable_commission());
  core::toProto(Report.Costs.Fee, Costs->mutable_exchange_fee());
  core::toProto(Report.Costs.Spread, Costs->mutable_spread());
  core::toProto(Report.Costs.Slippage, Costs->mutable_slippage());
  core::toProto(Report.Costs.OvernightFee, Costs->mutable_overnight_fee());
  core::toProto(Report.Costs.Total, Costs->mutable_total());
  for (const core::UnderwaterPoint &P : Report.Underwater) {
    pb::UnderwaterPoint *Point = Out->add_underwater();
    Point->set_step(P.Step);
    core::toProto(P.DrawdownPct, Point->mutable_drawdown_pct());
  }
  for (const core::EquityPoint &P : Report.Equity) {
    pb::EquityPoint *Point = Out->add_equity();
    Point->set_step(P.Step);
    core::toProto(P.Equity, Point->mutable_equity());
  }
  Out->set_level(toProto(Report.ReportLevel));
  for (const core::Finding &F : Report.Findings)
    toProto(F, Out->add_findings());
  core::toProto(Report.FinalEquity, Out->mutable_final_equity());
  for (const core::EquityPoint &P : Report.BenchmarkEquity) {
    pb::EquityPoint *Point = Out->add_benchmark_equity();
    Point->set_step(P.Step);
    core::toProto(P.Equity, Point->mutable_equity());
  }
  for (const core::EquityPoint &P : Report.BuyHoldEquity) {
    pb::EquityPoint *Point = Out->add_buy_hold_equity();
    Point->set_step(P.Step);
    core::toProto(P.Equity, Point->mutable_equity());
  }
  for (const core::EquityPoint &P : Report.RuoniaEquity) {
    pb::EquityPoint *Point = Out->add_ruonia_equity();
    Point->set_step(P.Step);
    core::toProto(P.Equity, Point->mutable_equity());
  }
  if (Report.BuyHoldReturnPct)
    core::toProto(*Report.BuyHoldReturnPct, Out->mutable_buy_hold_return_pct());
  if (Report.RuoniaReturnPct)
    core::toProto(*Report.RuoniaReturnPct, Out->mutable_ruonia_return_pct());
  std::int64_t TripSeq = 0;
  for (const core::RoundTrip &T : Report.RoundTrips) {
    pb::RoundTrip *Trip = Out->add_round_trips();
    Trip->set_seq(TripSeq++);
    Trip->set_instrument_key(Timeline.instrumentKey(T.Instrument));
    Trip->set_entry_step(T.EntryStep);
    Trip->set_exit_step(T.ExitStep);
    Trip->set_lots(T.Lots);
    core::toProto(T.EntryPrice, Trip->mutable_entry_price());
    core::toProto(T.ExitPrice, Trip->mutable_exit_price());
    core::toProto(T.grossPnl(), Trip->mutable_gross_pnl());
    core::toProto(T.Costs, Trip->mutable_costs());
    core::toProto(T.NetPnl, Trip->mutable_net_pnl());
  }
}

} // namespace worker
