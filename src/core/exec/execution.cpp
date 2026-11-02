#include "core/exec/execution.hpp"

#include <utility>

namespace core {

ExecutionEngine::ExecutionEngine(std::vector<Instrument> instruments, CostConfig costs, bool naive)
    : instruments_(std::move(instruments)), costs_(std::move(costs)), naive_(naive) {}

void ExecutionEngine::processStep(std::int64_t step, const Timeline& timeline, std::vector<Order>& orders,
                                  Portfolio& portfolio, std::vector<Fill>& fills,
                                  std::vector<std::int64_t>& changed) const {
  // Порядок на шаге: рыночные -> стоп-типы -> лимитные и тейк-профит (последние две группы - задача 14).
  // Бюджет ликвидности один на шаг: условные заявки видят то, что уже израсходовали рыночные.
  LiquidityBudget budget(instruments_.size());
  processMarket(step, timeline, budget, orders, portfolio, fills, changed);
}

std::optional<std::int64_t> LiquidityBudget::remaining(int instrument, const StepBar& bar, const CostConfig& costs) {
  if (!costs.liquidityShare) return std::nullopt;
  auto& left = left_.at(static_cast<std::size_t>(instrument));
  if (!left) {
    // floor(p·V) лотов.
    left = bar.volumeLots <= 0 ? 0
                               : static_cast<std::int64_t>(
                                     costs.liquidityShare->times(bar.volumeLots).round(0, RoundingMode::FLOOR).raw() /
                                     Decimal::kUnit);
  }
  return left;
}

void LiquidityBudget::consume(int instrument, std::int64_t lots) {
  auto& left = left_.at(static_cast<std::size_t>(instrument));
  if (left) *left -= lots;  // не задан - без лимита (remaining не вызывался) или бюджета не было
}

}  // namespace core
