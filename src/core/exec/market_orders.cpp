#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>

#include "core/exec/execution.hpp"
#include "exec_common.hpp"

namespace core {

namespace {

// Общий обход рыночных заявок. naive = false: заявки, поданные раньше шага, по
// open бара шага своей бумаги, с издержками и бюджетом ликвидности. naive =
// true (ФТ-24): заявки, поданные не позже шага, по close последнего бара бумаги
// без издержек и лимита. budget - бюджет ликвидности шага (nullptr в режиме
// naive).
void runMarketOrders(bool Naive, std::int64_t Step,
                     const std::vector<Instrument> &Instruments,
                     const CostConfig &Costs, const Timeline &Timeline,
                     LiquidityBudget *Budget, std::vector<Order> &Orders,
                     Portfolio &Portfolio, std::vector<Fill> &Fills,
                     std::vector<std::int64_t> &Changed) {
  const CostConfig ZeroCosts = CostConfig::zero();
  const CostConfig &Cfg = Naive ? ZeroCosts : Costs;

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
    if (Naive ? O.PlacedStep > Step : O.PlacedStep >= Step)
      continue; // INV-2: не раньше следующего бара

    const int InstrIdx = O.Spec.Instrument;
    const Instrument &Inst = Instruments[static_cast<std::size_t>(InstrIdx)];
    // Заявка ждёт бара своей бумаги: чужие бары шага ей не подходят.
    const StepBar *Bar = Naive ? Timeline.lastBarOf(InstrIdx, Step)
                               : Timeline.barOf(InstrIdx, Step);
    if (Bar == nullptr)
      continue;

    const Decimal Base = Naive ? Bar->Close : Bar->Open;
    // nullopt - без лимита (ФТ-24 и liquidityShare не задан); бар без объёма
    // при заданном лимите даёт 0.
    const std::optional<std::int64_t> Cap =
        Budget != nullptr ? Budget->remaining(InstrIdx, *Bar, Costs)
                          : std::nullopt;

    auto QuoteAt = [&](std::int64_t Q) {
      const Decimal Price =
          Naive ? Base
                : fillPrice(O.Spec.OrderSide, Base, Q, Bar->VolumeLots, Cfg,
                            Inst.PriceStep);
      return Quote{Price, amounts(Price, Q, Inst.LotSize, Cfg)};
    };
    const std::int64_t Remaining = O.Spec.Lots - O.FilledLots;
    const auto [q, reason] =
        chooseQuantity(O.Spec.OrderSide, Remaining, Cap,
                       Portfolio.lots(InstrIdx), Portfolio.cash(), QuoteAt);

    if (q > 0) {
      const Quote X = QuoteAt(q);
      recordFill(Portfolio, Fills, O, Step, q, X, Base, Inst, Cfg);
      O.FilledLots += q;
      if (Budget != nullptr)
        Budget->consume(InstrIdx, q);
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
    Changed.push_back(O.Id);
  }
}

} // namespace

void ExecutionEngine::processMarket(std::int64_t Step, const Timeline &Timeline,
                                    LiquidityBudget &Budget,
                                    std::vector<Order> &Orders,
                                    Portfolio &Portfolio,
                                    std::vector<Fill> &Fills,
                                    std::vector<std::int64_t> &Changed) const {
  runMarketOrders(false, Step, Instruments, Costs, Timeline, &Budget, Orders,
                  Portfolio, Fills, Changed);
}

void ExecutionEngine::processNaive(std::int64_t Step, const Timeline &Timeline,
                                   std::vector<Order> &Orders,
                                   Portfolio &Portfolio,
                                   std::vector<Fill> &Fills,
                                   std::vector<std::int64_t> &Changed) const {
  if (!Naive)
    throw std::logic_error(
        "processNaive: движок создан без режима naive (ФТ-24)");
  runMarketOrders(true, Step, Instruments, Costs, Timeline, nullptr, Orders,
                  Portfolio, Fills, Changed);
}

} // namespace core
