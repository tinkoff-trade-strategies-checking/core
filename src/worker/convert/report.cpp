#include "convert/report.hpp"

#include <cstddef>
#include <utility>
#include <vector>

#include "convert/diagnostics.hpp"
#include "core/money/proto_conv.hpp"

namespace worker {

namespace {

// Номера торговых дней шагов 0…lastStep (в шкале с 1).
std::vector<std::int32_t> tradingDays(const core::Timeline &Timeline,
                                      std::int64_t LastStep) {
  std::vector<std::int32_t> Days;
  Days.reserve(static_cast<std::size_t>(LastStep) + 1);
  for (std::int64_t Step = 0; Step <= LastStep; ++Step)
    Days.push_back(Timeline.tradingDayOf(Step));
  return Days;
}

void countOrders(const std::vector<core::Order> &Orders,
                 core::MetricsInput *In) {
  for (const core::Order &O : Orders) {
    if (O.Status == core::OrderStatus::Rejected)
      ++In->Rejections;
    if (O.FilledLots > 0 && O.FilledLots < O.Spec.Lots)
      ++In->PartialFills;
  }
}

// Доходность бенчмарка за период и кривая до последнего шага кривой стоимости
// (только полностью пройденные дни).
void setBenchmark(const std::optional<core::BenchmarkSeries> &Benchmark,
                  const core::Timeline &Timeline, core::MetricsInput *In) {
  if (!Benchmark)
    return;
  In->Benchmark = core::benchmarkRange(*Benchmark);
  // open первого дня <= 0 - испорченный бар индекса: доходности нет
  // (computeReport), кривой тоже, отчёт считается.
  if (In->Benchmark && In->Benchmark->first > core::Decimal::zero() &&
      !In->Equity.empty()) {
    In->BenchmarkEquity = core::benchmarkEquity(
        *Benchmark, In->InitialCash, Timeline, In->Equity.back().Step);
  }
}

void metricsToProto(const core::Metrics &M, pb::Metrics *Out) {
  Out->set_total_return(M.TotalReturn);
  Out->set_gross_return(M.GrossReturn);
  if (M.Cagr)
    Out->set_cagr(*M.Cagr);
  Out->set_max_drawdown(M.MaxDrawdown);
  Out->set_max_drawdown_steps(M.MaxDrawdownSteps);
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
             const std::optional<core::BenchmarkSeries> &Benchmark) {
  core::MetricsInput In;
  In.InitialCash = InitialCash;
  In.Equity = Engine.equity();
  In.TradingDayOfStep = tradingDays(Timeline, Engine.currentStep());
  In.Fills = Engine.fills();
  In.RoundTrips = Engine.portfolio().roundTrips();
  countOrders(Engine.orders(), &In);
  In.PositionValuesByStep = Engine.positionValues();
  setBenchmark(Benchmark, Timeline, &In);
  return In;
}

core::MetricsInput
metricsInput(const core::RunOutput &Output, const core::Decimal &InitialCash,
             const core::Timeline &Timeline,
             const std::optional<core::BenchmarkSeries> &Benchmark) {
  core::MetricsInput In;
  In.InitialCash = InitialCash;
  In.Equity = Output.Equity;
  In.TradingDayOfStep = tradingDays(Timeline, Output.LastStep);
  In.Fills = Output.Fills;
  In.RoundTrips = Output.RoundTrips;
  In.Rejections = Output.Rejections;
  In.PartialFills = Output.PartialFills;
  In.PositionValuesByStep = Output.PositionValuesByStep;
  setBenchmark(Benchmark, Timeline, &In);
  return In;
}

void toProto(const core::Report &Report, pb::Report *Out) {
  metricsToProto(Report.Stats, Out->mutable_metrics());
  pb::CostTotals *Costs = Out->mutable_costs();
  core::toProto(Report.Costs.Commission, Costs->mutable_commission());
  core::toProto(Report.Costs.Fee, Costs->mutable_exchange_fee());
  core::toProto(Report.Costs.Spread, Costs->mutable_spread());
  core::toProto(Report.Costs.Slippage, Costs->mutable_slippage());
  for (const core::EquityPoint &P : Report.Equity) {
    pb::EquityPoint *Point = Out->add_equity();
    Point->set_step(P.Step);
    core::toProto(P.Equity, Point->mutable_equity());
  }
  Out->set_level(pb::OK);
  core::toProto(Report.FinalEquity, Out->mutable_final_equity());
  for (const core::EquityPoint &P : Report.BenchmarkEquity) {
    pb::EquityPoint *Point = Out->add_benchmark_equity();
    Point->set_step(P.Step);
    core::toProto(P.Equity, Point->mutable_equity());
  }
}

void toProto(const core::Finding &Finding, pb::Finding *Out) {
  Out->set_code(Finding.Code);
  Out->set_level(convert::toProto(Finding.Severity));
  Out->set_message(Finding.Message);
  Out->set_step(Finding.Step);
}

} // namespace worker
