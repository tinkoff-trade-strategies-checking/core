#include "convert/run.hpp"

#include "convert/diagnostics.hpp"
#include "convert/report.hpp"
#include "convert/state.hpp"
#include "core/money/proto_conv.hpp"

namespace worker {

void toProto(const core::RunOutput &Output, const core::Report &Report,
             const std::optional<core::CostBreakdown> &CostBreakdown,
             const core::Timeline &Timeline, pb::RunResult *Out) {
  Out->set_status(toProto(Output.Status));
  Out->set_level(convert::toProto(Output.Severity));
  pb::Report *R = Out->mutable_report();
  toProto(Report, R);
  R->set_level(convert::toProto(Output.Severity));
  for (const core::Finding &F : Output.Findings)
    toProto(F, R->add_findings());
  for (const core::Fill &F : Output.Fills)
    toProto(F, Timeline, Out->add_fills());
  for (const core::Order &O : Output.Orders)
    toProto(O, Timeline, Out->add_orders());
  if (CostBreakdown) {
    pb::CostBreakdown *Cb = Out->mutable_cost_breakdown();
    for (const core::CostBreakdownStep &S : CostBreakdown->Steps) {
      pb::CostBreakdownStep *Step = Cb->add_steps();
      Step->set_factor(toProto(S.Factor));
      core::toProto(S.NetResult, Step->mutable_net_result());
      Step->set_net_return(S.NetReturn);
    }
    for (const core::CostFlag Flag : CostBreakdown->Flags)
      Cb->add_flags(toProto(Flag));
  }
  Out->set_digest(Output.Digest);
  Out->set_last_step(Output.LastStep);
  Out->set_total_steps(Timeline.totalSteps());
}

pb::RunStatus toProto(core::RunStatus Status) {
  switch (Status) {
  case core::RunStatus::Completed:
    return pb::COMPLETED;
  case core::RunStatus::BankruptStop:
    return pb::BANKRUPT_STOP;
  case core::RunStatus::Invalid:
    return pb::INVALID;
  }
  return pb::RUN_STATUS_UNSPECIFIED;
}

pb::CostFactor toProto(core::CostFactor Factor) {
  switch (Factor) {
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

pb::CostFlag toProto(core::CostFlag Flag) {
  switch (Flag) {
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
