#include "core/analysis/cost_breakdown.hpp"

#include <stdexcept>
#include <utility>

#include "core/run/backtest.hpp"

namespace core {

namespace {

constexpr double kSharpeLimit = 3.0;

CostBreakdownStep stepOf(CostFactor Factor, CostStageRun &Stage,
                         const SessionSpec &Spec,
                         const std::shared_ptr<const Timeline> &Timeline,
                         const dsl::Program &Program,
                         const std::vector<dsl::ResolvedParam> &Params,
                         const dsl::ParamValues &Overrides) {
  const RunOutput Out = runBacktest(Spec, Timeline, Program, Params, Overrides,
                                    dsl::Level::Ok, nullptr);
  Stage = CostStageRun{Out.Status, Out.Severity, Out.Findings};
  const Decimal Net = Out.FinalEquity - Spec.InitialCash;
  return CostBreakdownStep{Factor, Net,
                           Net.toDouble() / Spec.InitialCash.toDouble()};
}

} // namespace

std::vector<CostFlag> costFlags(const Decimal &NaiveNet,
                                const Decimal &HonestNet,
                                std::optional<double> HonestSharpe) {
  std::vector<CostFlag> Flags;
  // R_наивн - R_честн > 0,5 * R_наивн без деления: 2 * (R_наивн - R_честн) >
  // R_наивн.
  const Decimal Lost = NaiveNet - HonestNet;
  if (NaiveNet.sign() > 0 && Lost + Lost > NaiveNet)
    Flags.push_back(CostFlag::CostsOverHalfProfit);
  if (NaiveNet.sign() != HonestNet.sign())
    Flags.push_back(CostFlag::SignChange);
  if (HonestSharpe && *HonestSharpe > kSharpeLimit)
    Flags.push_back(CostFlag::SharpeOver3);
  return Flags;
}

CostBreakdown costBreakdown(const SessionSpec &Honest,
                            std::shared_ptr<const Timeline> Timeline,
                            const dsl::Program &Program,
                            const std::vector<dsl::ResolvedParam> &Params,
                            const dsl::ParamValues &Overrides,
                            std::optional<double> HonestSharpe,
                            const StageDone &AfterStage) {
  return costBreakdownDetailed(Honest, std::move(Timeline), Program, Params,
                               Overrides, HonestSharpe, AfterStage)
      .Breakdown;
}

CostBreakdownDetail costBreakdownDetailed(
    const SessionSpec &Honest, std::shared_ptr<const Timeline> Timeline,
    const dsl::Program &Program, const std::vector<dsl::ResolvedParam> &Params,
    const dsl::ParamValues &Overrides, std::optional<double> HonestSharpe,
    const StageDone &AfterStage) {
  if (Honest.Naive)
    throw std::invalid_argument(
        "costBreakdown: честный прогон не может быть наивным (ФТ-24)");

  SessionSpec Spec = Honest;
  CostBreakdownDetail Detail;
  CostBreakdown &Result = Detail.Breakdown;
  const auto Add = [&](CostFactor Factor) {
    CostStageRun Stage;
    Result.Steps.push_back(
        stepOf(Factor, Stage, Spec, Timeline, Program, Params, Overrides));
    Detail.Stages.push_back(std::move(Stage));
    if (AfterStage)
      AfterStage();
  };

  Spec.Naive = true;
  Spec.Costs = CostConfig::zero();
  Add(CostFactor::Naive);

  Spec.Naive = false;
  Add(CostFactor::Delay);

  Spec.Costs.CommissionRate = Honest.Costs.CommissionRate;
  Spec.Costs.ExchangeFeeRate = Honest.Costs.ExchangeFeeRate;
  Add(CostFactor::Fees);

  Spec.Costs.Spread = Honest.Costs.Spread;
  Add(CostFactor::Spread);

  Spec.Costs.SlippageKappa = Honest.Costs.SlippageKappa;
  Add(CostFactor::Slippage);

  Spec.Costs = Honest.Costs; // INV-3: последний этап - ровно честные настройки
  Add(CostFactor::Liquidity);

  Result.Flags = costFlags(Result.Steps.front().NetResult,
                           Result.Steps.back().NetResult, HonestSharpe);
  return Detail;
}

} // namespace core
