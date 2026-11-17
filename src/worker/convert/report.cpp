#include "convert/report.hpp"

#include <cstddef>
#include <utility>
#include <vector>

#include "convert/diagnostics.hpp"
#include "core/money/proto_conv.hpp"

namespace worker {

namespace {

// Номера торговых дней шагов 0…lastStep (в шкале с 1).
std::vector<std::int32_t> tradingDays(const core::Timeline &timeline,
                                      std::int64_t lastStep) {
  std::vector<std::int32_t> days;
  days.reserve(static_cast<std::size_t>(lastStep) + 1);
  for (std::int64_t step = 0; step <= lastStep; ++step)
    days.push_back(timeline.tradingDayOf(step));
  return days;
}

void countOrders(const std::vector<core::Order> &orders,
                 core::MetricsInput *in) {
  for (const core::Order &o : orders) {
    if (o.status == core::OrderStatus::Rejected)
      ++in->rejections;
    if (o.filledLots > 0 && o.filledLots < o.spec.lots)
      ++in->partialFills;
  }
}

// Доходность бенчмарка за период и кривая до последнего шага кривой стоимости
// (только полностью пройденные дни).
void setBenchmark(const std::optional<core::BenchmarkSeries> &benchmark,
                  const core::Timeline &timeline, core::MetricsInput *in) {
  if (!benchmark)
    return;
  in->benchmark = core::benchmarkRange(*benchmark);
  // open первого дня <= 0 - испорченный бар индекса: доходности нет
  // (computeReport), кривой тоже, отчёт считается.
  if (in->benchmark && in->benchmark->first > core::Decimal::zero() &&
      !in->equity.empty()) {
    in->benchmarkEquity = core::benchmarkEquity(
        *benchmark, in->initialCash, timeline, in->equity.back().step);
  }
}

void metricsToProto(const core::Metrics &m, pb::Metrics *out) {
  out->set_total_return(m.totalReturn);
  out->set_gross_return(m.grossReturn);
  if (m.cagr)
    out->set_cagr(*m.cagr);
  out->set_max_drawdown(m.maxDrawdown);
  out->set_max_drawdown_steps(m.maxDrawdownSteps);
  if (m.volatility)
    out->set_volatility(*m.volatility);
  if (m.sharpe)
    out->set_sharpe(*m.sharpe);
  if (m.sortino)
    out->set_sortino(*m.sortino);
  out->set_turnover(m.turnover);
  out->set_fills(m.fills);
  out->set_round_trips(m.roundTrips);
  if (m.winRate)
    out->set_win_rate(*m.winRate);
  out->set_rejections(m.rejections);
  out->set_partial_fills(m.partialFills);
  out->set_hhi_avg(m.hhiAvg);
  out->set_max_position_share(m.maxPositionShare);
  if (m.benchmarkReturn)
    out->set_benchmark_return(*m.benchmarkReturn);
  out->set_trading_days(m.tradingDays);
  out->set_stop_take_same_bar(m.stopTakeSameBar);
}

} // namespace

core::MetricsInput
metricsInput(const core::SessionEngine &engine,
             const core::Decimal &initialCash, const core::Timeline &timeline,
             const std::optional<core::BenchmarkSeries> &benchmark) {
  core::MetricsInput in;
  in.initialCash = initialCash;
  in.equity = engine.equity();
  in.tradingDayOfStep = tradingDays(timeline, engine.currentStep());
  in.fills = engine.fills();
  in.roundTrips = engine.portfolio().roundTrips();
  countOrders(engine.orders(), &in);
  in.positionValuesByStep = engine.positionValues();
  setBenchmark(benchmark, timeline, &in);
  return in;
}

core::MetricsInput
metricsInput(const core::RunOutput &output, const core::Decimal &initialCash,
             const core::Timeline &timeline,
             const std::optional<core::BenchmarkSeries> &benchmark) {
  core::MetricsInput in;
  in.initialCash = initialCash;
  in.equity = output.equity;
  in.tradingDayOfStep = tradingDays(timeline, output.lastStep);
  in.fills = output.fills;
  in.roundTrips = output.roundTrips;
  in.rejections = output.rejections;
  in.partialFills = output.partialFills;
  in.positionValuesByStep = output.positionValuesByStep;
  setBenchmark(benchmark, timeline, &in);
  return in;
}

void toProto(const core::Report &report, pb::Report *out) {
  metricsToProto(report.metrics, out->mutable_metrics());
  pb::CostTotals *costs = out->mutable_costs();
  core::toProto(report.costs.commission, costs->mutable_commission());
  core::toProto(report.costs.fee, costs->mutable_exchange_fee());
  core::toProto(report.costs.spread, costs->mutable_spread());
  core::toProto(report.costs.slippage, costs->mutable_slippage());
  for (const core::EquityPoint &p : report.equity) {
    pb::EquityPoint *point = out->add_equity();
    point->set_step(p.step);
    core::toProto(p.equity, point->mutable_equity());
  }
  out->set_level(pb::OK);
  core::toProto(report.finalEquity, out->mutable_final_equity());
  for (const core::EquityPoint &p : report.benchmarkEquity) {
    pb::EquityPoint *point = out->add_benchmark_equity();
    point->set_step(p.step);
    core::toProto(p.equity, point->mutable_equity());
  }
}

void toProto(const core::Finding &finding, pb::Finding *out) {
  out->set_code(finding.code);
  out->set_level(convert::toProto(finding.level));
  out->set_message(finding.message);
  out->set_step(finding.step);
}

} // namespace worker
