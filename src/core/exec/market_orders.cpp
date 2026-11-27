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
                     const CostConfig &Costs, const MarginConfig &Margin,
                     const ExecutionRules &Rules, const Timeline &Timeline,
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

    const std::optional<Decimal> AuctionPrice =
        auctionClearingPrice(Timeline, Step, *Bar, Rules);
    if (O.Spec.AuctionOnly && !AuctionPrice) {
      if (O.Spec.Tif == TimeInForce::Ioc || O.Spec.Tif == TimeInForce::Fok) {
        O.Status = OrderStatus::Cancelled;
        O.ReasonCode = O.Spec.Tif == TimeInForce::Fok
                           ? Reason::FillOrKill
                           : Reason::ImmediateOrCancel;
        O.ClosedStep = Step;
        markChanged(Changed, ChangedFrom, O.Id);
      }
      continue;
    }

    const std::optional<PriceBand> Band =
        priceBandAt(Timeline, InstrIdx, Step, Inst.PriceStep, Rules);
    const Decimal Base = AuctionPrice ? *AuctionPrice : Bar->Open;
    // nullopt - без лимита (liquidityShare не задан); бар без объёма при
    // заданном лимите даёт 0.
    const std::optional<std::int64_t> Cap =
        Budget.remaining(InstrIdx, *Bar, Costs);

    auto QuoteAt = [&](std::int64_t Q) {
      Decimal Price =
          AuctionPrice
              ? *AuctionPrice
              : fillPrice(O.Spec.OrderSide, Base, Q, Bar->VolumeLots, Costs,
                          Inst.PriceStep);
      if (Band)
        Price = std::clamp(Price, Band->Lower, Band->Upper);
      return Quote{Price, amounts(Price, Q, Inst.LotSize, Costs)};
    };
    const std::int64_t Remaining = O.Spec.Lots - O.FilledLots;
    const bool IsFok = O.Spec.Tif == TimeInForce::Fok;
    const bool IsIoc = O.Spec.Tif == TimeInForce::Ioc;
    const std::int64_t TargetLots =
        (O.Spec.DisplayLots > 0 && !IsFok)
            ? std::min(Remaining, O.Spec.DisplayLots)
            : Remaining;
    const std::int64_t PosLots = Portfolio.lots(InstrIdx);
    const Decimal &Cash = Portfolio.cash();
    std::pair<std::int64_t, Reason> Chosen;
    if (Margin.isMarginEnabled()) {
      const auto [OtherVal, OtherGross] =
          otherExposure(Portfolio, Instruments, Timeline, Step, InstrIdx);
      Chosen = chooseMarginQuantity(O.Spec.OrderSide, TargetLots, Cap, PosLots,
                                    Cash, Inst.LotSize, OtherVal, OtherGross,
                                    Margin, QuoteAt);
    } else {
      Chosen = chooseQuantity(O.Spec.OrderSide, TargetLots, Cap, PosLots, Cash,
                              QuoteAt);
    }
    std::int64_t Q = Chosen.first;
    Reason ReasonCode = Chosen.second;
    if (IsFok && (Q < Remaining || ReasonCode != Reason::None)) {
      Q = 0;
      ReasonCode = Reason::FillOrKill;
    }

    if (Q > 0) {
      const Quote X = QuoteAt(Q);
      Fill &F = recordFill(Portfolio, Fills, O, Step, Q, X, Base, Inst, Costs);
      if (AuctionPrice)
        F.SpreadCost = F.SlippageCost = Decimal::zero();
      O.FilledLots += Q;
      Budget.consume(InstrIdx, Q);
    }

    if (O.FilledLots >= O.Spec.Lots) {
      O.Status = OrderStatus::Filled;
      O.ClosedStep = Step;
    } else if (IsFok) {
      O.Status = OrderStatus::Cancelled;
      O.ReasonCode = Reason::FillOrKill;
      O.ClosedStep = Step;
    } else if (IsIoc) {
      O.Status = OrderStatus::Cancelled;
      O.ReasonCode = (ReasonCode == Reason::None ||
                      ReasonCode == Reason::LiquidityLimit)
                         ? Reason::ImmediateOrCancel
                         : ReasonCode;
      O.ClosedStep = Step;
    } else if (O.Spec.DisplayLots > 0 &&
               (ReasonCode == Reason::None ||
                ReasonCode == Reason::LiquidityLimit)) {
      if (Q > 0)
        O.Status = OrderStatus::PartiallyFilled;
    } else {
      O.Status = OrderStatus::Cancelled;
      O.ReasonCode = ReasonCode;
      O.ClosedStep = Step;
    }
    markChanged(Changed, ChangedFrom, O.Id);
    if (!Margin.AllowShort && O.Spec.OrderSide == Side::Sell && Q > 0 &&
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
  runMarketOrders(Step, Instruments, Costs, Margin, Rules, Timeline, Budget,
                  Orders, Portfolio, Fills, Changed);
}

} // namespace core
