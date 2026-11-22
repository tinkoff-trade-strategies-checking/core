#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <utility>

#include "core/exec/execution.hpp"
#include "exec_common.hpp"

namespace core {

namespace {

// Рыночные заявки, поданные раньше шага, по open бара шага своей бумаги, с
// издержками и бюджетом ликвидности.
void runMarketOrders(std::int64_t Step,
                     const std::vector<Instrument> &Instruments,
                     const CostConfig &Costs, const Timeline &Timeline,
                     LiquidityBudget &Budget, std::vector<Order> &Orders,
                     Portfolio &Portfolio, std::vector<Fill> &Fills,
                     std::vector<std::int64_t> &Changed) {
  const std::size_t ChangedFrom = Changed.size();
  // Заявки - по возрастанию id.
  std::vector<std::size_t> ByOrderId(Orders.size());
  std::iota(ByOrderId.begin(), ByOrderId.end(), std::size_t{0});
  std::stable_sort(ByOrderId.begin(), ByOrderId.end(),
                   [&](std::size_t A, std::size_t B) {
                     return Orders[A].Id < Orders[B].Id;
                   });

  for (const std::size_t Index : ByOrderId) {
    Order &O = Orders[Index];
    if (O.Spec.Type != OrderType::Market)
      continue;
    if (O.Status != OrderStatus::Active &&
        O.Status != OrderStatus::PartiallyFilled)
      continue;
    if (O.PlacedStep >= Step)
      continue; // INV-2: не раньше следующего бара

    const int InstrIdx = O.Spec.Instrument;
    const Instrument &Inst = Instruments[static_cast<std::size_t>(InstrIdx)];
    // Заявка ждёт бара своей бумаги: чужие бары шага ей не подходят.
    const StepBar *Bar = Timeline.barOf(InstrIdx, Step);
    if (Bar == nullptr)
      continue;

    const Decimal Base = Bar->Open;
    // nullopt - без лимита (liquidityShare не задан); бар без объёма при
    // заданном лимите даёт 0.
    const std::optional<std::int64_t> Cap =
        Budget.remaining(InstrIdx, *Bar, Costs);

    auto QuoteAt = [&](std::int64_t Q) {
      const Decimal Price = fillPrice(O.Spec.OrderSide, Base, Q,
                                      Bar->VolumeLots, Costs, Inst.PriceStep);
      return Quote{Price, amounts(Price, Q, Inst.LotSize, Costs)};
    };
    const std::int64_t Remaining = O.Spec.Lots - O.FilledLots;
    const auto [q, reason] =
        chooseQuantity(O.Spec.OrderSide, Remaining, Cap,
                       Portfolio.lots(InstrIdx), Portfolio.cash(), QuoteAt);

    if (q > 0) {
      const Quote X = QuoteAt(q);
      recordFill(Portfolio, Fills, O, Step, q, X, Base, Inst, Costs);
      O.FilledLots += q;
      Budget.consume(InstrIdx, q);
    }
    // Остаток рыночной заявки не переносится: заявка исполнена целиком или
    // снята с причиной.
    if (reason == Reason::None) {
      O.Status = OrderStatus::Filled;
    } else {
      O.Status = OrderStatus::Cancelled;
      O.ReasonCode = reason;
    }
    O.ClosedStep = Step;
    markChanged(Changed, ChangedFrom, O.Id);
    if (O.Spec.OrderSide == Side::Sell && q > 0 &&
        Portfolio.lots(InstrIdx) == 0) {
      cancelSellOrdersOnPositionClosed(Orders, InstrIdx, Step, Changed,
                                       ChangedFrom);
    }
  }
}

} // namespace

void ExecutionEngine::processMarket(std::int64_t Step, const Timeline &Timeline,
                                    LiquidityBudget &Budget,
                                    std::vector<Order> &Orders,
                                    Portfolio &Portfolio,
                                    std::vector<Fill> &Fills,
                                    std::vector<std::int64_t> &Changed) const {
  runMarketOrders(Step, Instruments, Costs, Timeline, Budget, Orders, Portfolio,
                  Fills, Changed);
}

} // namespace core
