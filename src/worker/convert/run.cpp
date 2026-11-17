#include "convert/run.hpp"

#include "convert/diagnostics.hpp"
#include "convert/report.hpp"
#include "convert/state.hpp"
#include "core/money/proto_conv.hpp"

namespace worker {

void toProto(const core::RunOutput &output, const core::Report &report,
             const std::optional<core::CostBreakdown> &costBreakdown,
             const core::Timeline &timeline, pb::RunResult *out) {
  out->set_status(toProto(output.status));
  out->set_level(convert::toProto(output.level));
  pb::Report *r = out->mutable_report();
  toProto(report, r);
  r->set_level(convert::toProto(output.level));
  for (const core::Finding &f : output.findings)
    toProto(f, r->add_findings());
  for (const core::Fill &f : output.fills)
    toProto(f, timeline, out->add_fills());
  for (const core::Order &o : output.orders)
    toProto(o, timeline, out->add_orders());
  if (costBreakdown) {
    pb::CostBreakdown *cb = out->mutable_cost_breakdown();
    for (const core::CostBreakdownStep &s : costBreakdown->steps) {
      pb::CostBreakdownStep *step = cb->add_steps();
      step->set_factor(toProto(s.factor));
      core::toProto(s.netResult, step->mutable_net_result());
      step->set_net_return(s.netReturn);
    }
    for (const core::CostFlag flag : costBreakdown->flags)
      cb->add_flags(toProto(flag));
  }
  out->set_digest(output.digest);
  out->set_last_step(output.lastStep);
  out->set_total_steps(timeline.totalSteps());
}

pb::RunStatus toProto(core::RunStatus status) {
  switch (status) {
  case core::RunStatus::Completed:
    return pb::COMPLETED;
  case core::RunStatus::BankruptStop:
    return pb::BANKRUPT_STOP;
  case core::RunStatus::Invalid:
    return pb::INVALID;
  }
  return pb::RUN_STATUS_UNSPECIFIED;
}

pb::CostFactor toProto(core::CostFactor factor) {
  switch (factor) {
  case core::CostFactor::Naive:
    return pb::NAIVE;
  case core::CostFactor::Delay:
    return pb::DELAY;
  case core::CostFactor::Fees:
    return pb::FEES;
  case core::CostFactor::Spread:
    return pb::SPREAD;
  case core::CostFactor::Slippage:
    return pb::SLIPPAGE;
  case core::CostFactor::Liquidity:
    return pb::LIQUIDITY;
  }
  return pb::COST_FACTOR_UNSPECIFIED;
}

pb::CostFlag toProto(core::CostFlag flag) {
  switch (flag) {
  case core::CostFlag::CostsOverHalfProfit:
    return pb::COSTS_OVER_HALF_PROFIT;
  case core::CostFlag::SignChange:
    return pb::SIGN_CHANGE;
  case core::CostFlag::SharpeOver3:
    return pb::SHARPE_OVER_3;
  }
  return pb::COST_FLAG_UNSPECIFIED;
}

} // namespace worker
