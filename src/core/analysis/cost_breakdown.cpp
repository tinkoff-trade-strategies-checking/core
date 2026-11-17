#include "core/analysis/cost_breakdown.hpp"

#include <stdexcept>
#include <utility>

#include "core/run/backtest.hpp"

namespace core {

namespace {

constexpr double kSharpeLimit = 3.0;

CostBreakdownStep stepOf(CostFactor factor, CostStageRun &stage,
                         const SessionSpec &spec,
                         const std::shared_ptr<const Timeline> &timeline,
                         const dsl::Program &program,
                         const std::vector<dsl::ResolvedParam> &params,
                         const dsl::ParamValues &overrides) {
  const RunOutput out = runBacktest(spec, timeline, program, params, overrides,
                                    dsl::Level::Ok, nullptr);
  stage = CostStageRun{out.status, out.level, out.findings};
  const Decimal net = out.finalEquity - spec.initialCash;
  return CostBreakdownStep{factor, net,
                           net.toDouble() / spec.initialCash.toDouble()};
}

} // namespace

std::vector<CostFlag> costFlags(const Decimal &naiveNet,
                                const Decimal &honestNet,
                                std::optional<double> honestSharpe) {
  std::vector<CostFlag> flags;
  // R_наивн - R_честн > 0,5 * R_наивн без деления: 2 * (R_наивн - R_честн) >
  // R_наивн.
  const Decimal lost = naiveNet - honestNet;
  if (naiveNet.sign() > 0 && lost + lost > naiveNet)
    flags.push_back(CostFlag::CostsOverHalfProfit);
  if (naiveNet.sign() != honestNet.sign())
    flags.push_back(CostFlag::SignChange);
  if (honestSharpe && *honestSharpe > kSharpeLimit)
    flags.push_back(CostFlag::SharpeOver3);
  return flags;
}

CostBreakdown costBreakdown(const SessionSpec &honest,
                            std::shared_ptr<const Timeline> timeline,
                            const dsl::Program &program,
                            const std::vector<dsl::ResolvedParam> &params,
                            const dsl::ParamValues &overrides,
                            std::optional<double> honestSharpe,
                            const StageDone &afterStage) {
  return costBreakdownDetailed(honest, std::move(timeline), program, params,
                               overrides, honestSharpe, afterStage)
      .breakdown;
}

CostBreakdownDetail costBreakdownDetailed(
    const SessionSpec &honest, std::shared_ptr<const Timeline> timeline,
    const dsl::Program &program, const std::vector<dsl::ResolvedParam> &params,
    const dsl::ParamValues &overrides, std::optional<double> honestSharpe,
    const StageDone &afterStage) {
  if (honest.naive)
    throw std::invalid_argument(
        "costBreakdown: честный прогон не может быть наивным (ФТ-24)");

  SessionSpec spec = honest;
  CostBreakdownDetail detail;
  CostBreakdown &result = detail.breakdown;
  const auto add = [&](CostFactor factor) {
    CostStageRun stage;
    result.steps.push_back(
        stepOf(factor, stage, spec, timeline, program, params, overrides));
    detail.stages.push_back(std::move(stage));
    if (afterStage)
      afterStage();
  };

  spec.naive = true;
  spec.costs = CostConfig::zero();
  add(CostFactor::Naive);

  spec.naive = false;
  add(CostFactor::Delay);

  spec.costs.commissionRate = honest.costs.commissionRate;
  spec.costs.exchangeFeeRate = honest.costs.exchangeFeeRate;
  add(CostFactor::Fees);

  spec.costs.spread = honest.costs.spread;
  add(CostFactor::Spread);

  spec.costs.slippageKappa = honest.costs.slippageKappa;
  add(CostFactor::Slippage);

  spec.costs = honest.costs; // INV-3: последний этап - ровно честные настройки
  add(CostFactor::Liquidity);

  result.flags = costFlags(result.steps.front().netResult,
                           result.steps.back().netResult, honestSharpe);
  return detail;
}

} // namespace core
