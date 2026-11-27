#include "core/exec/execution.hpp"

#include <utility>

namespace core {

ExecutionEngine::ExecutionEngine(std::vector<Instrument> InstrumentList,
                                 CostConfig CostModel, MarginConfig MarginModel,
                                 ExecutionRules RulesModel)
    : Instruments(std::move(InstrumentList)), Costs(std::move(CostModel)),
      Margin(std::move(MarginModel)), Rules(std::move(RulesModel)) {}

void ExecutionEngine::processStep(std::int64_t Step, const Timeline &Timeline,
                                  std::vector<Order> &Orders,
                                  Portfolio &Portfolio,
                                  std::vector<Fill> &Fills,
                                  std::vector<std::int64_t> &Changed) const {
  // Порядок на шаге: рыночные -> стоп-типы -> лимитные и тейк-профит; затем
  // снятие заявок «день» (всё - в processConditional). Бюджет ликвидности один
  // на шаг: условные заявки видят то, что уже израсходовали рыночные.
  LiquidityBudget Budget(Instruments.size());
  processMarket(Step, Timeline, Budget, Orders, Portfolio, Fills, Changed);
  processConditional(Step, Timeline, Budget, Orders, Portfolio, Fills, Changed);
}

std::optional<std::int64_t>
LiquidityBudget::remaining(int Instrument, const StepBar &Bar,
                           const CostConfig &Costs) {
  if (!Costs.LiquidityShare)
    return std::nullopt;
  auto &Slot = Left.at(static_cast<std::size_t>(Instrument));
  if (!Slot) {
    // floor(p·V) лотов.
    Slot = Bar.VolumeLots <= 0 ? 0
                               : static_cast<std::int64_t>(
                                     Costs.LiquidityShare->times(Bar.VolumeLots)
                                         .round(0, RoundingMode::FLOOR)
                                         .raw() /
                                     Decimal::kUnit);
  }
  return Slot;
}

void LiquidityBudget::consume(int Instrument, std::int64_t Lots) {
  auto &Slot = Left.at(static_cast<std::size_t>(Instrument));
  if (Slot)
    *Slot -= Lots; // не задан - без лимита (remaining не вызывался) или бюджета
                   // не было
}

} // namespace core
