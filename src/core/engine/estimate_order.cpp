#include "core/engine/session_engine.hpp"

#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>

#include "core/blind/blind.hpp"
#include "core/exec/costs.hpp"
#include "core/exec/execution.hpp"
#include "core/money/rounding.hpp"

namespace core {

OrderEstimate estimateOrder(const SessionSpec &Spec,
                            std::shared_ptr<const Timeline> TimelineRef,
                            std::span<const JournalEntry> Journal,
                            std::int64_t UpToStep, const OrderSpec &Order) {
  const ReplayResult Result =
      replaySession(Spec, TimelineRef, Journal, UpToStep,
                    std::numeric_limits<std::int64_t>::max());

  OrderEstimate Out;
  if (Result.Status != SessionStatus::Running) {
    Out.Check = Validation{false, Reason::SessionNotActive, ""};
    return Out;
  }

  const ResolvedOrderResult Resolved =
      resolveBlindOrderPrices(Order, *TimelineRef, Result.LastStep);
  if (!Resolved.Check.Ok) {
    Out.Check = Resolved.Check;
    return Out;
  }
  const OrderSpec &ResolvedOrder = *Resolved.Order;
  if (Order.IsBlindPrice)
    Out.ResolvedOrder = ResolvedOrder;

  const ExecutionEngine Exec(Spec.Instruments, Spec.Costs);
  Out.Check = Exec.validate(ResolvedOrder, Result.Engine.portfolio(),
                            *TimelineRef, Result.LastStep);

  if (ResolvedOrder.Instrument >= 0 &&
      static_cast<std::size_t>(ResolvedOrder.Instrument) <
          Spec.Instruments.size() &&
      ResolvedOrder.Lots >= 1) {
    const StepBar *Last =
        TimelineRef->lastBarOf(ResolvedOrder.Instrument, Result.LastStep);
    if (Last != nullptr) {
      const std::int64_t LotSize =
          Spec.Instruments[static_cast<std::size_t>(ResolvedOrder.Instrument)]
              .LotSize;
      Out.ReferencePrice = Last->Close;
      try {
        const Amounts A =
            amounts(Last->Close, ResolvedOrder.Lots, LotSize, Spec.Costs);
        const Decimal ReqCash =
            requiredCashForOrder(Last->Close, ResolvedOrder.Lots, LotSize,
                                 Spec.Costs)
                .round(2, RoundingMode::CEILING);
        const Decimal ValPct = toPercentOfCapital(A.Value, Spec.InitialCash);
        const Decimal CommPct =
            toPercentOfCapital(A.Commission, Spec.InitialCash);
        const Decimal FeePct = toPercentOfCapital(A.Fee, Spec.InitialCash);
        const Decimal ReqCashPct =
            toPercentOfCapital(ReqCash, Spec.InitialCash);

        Out.Value = A.Value;
        Out.Commission = A.Commission;
        Out.Fee = A.Fee;
        Out.RequiredCash = ReqCash;
        Out.ValuePct = ValPct;
        Out.CommissionPct = CommPct;
        Out.FeePct = FeePct;
        Out.RequiredCashPct = ReqCashPct;
      } catch (const DecimalOverflow &) {
        if (Out.Check.Ok)
          Out.Check = Validation{false, Reason::InsufficientCash, ""};
      }
    }
  }
  return Out;
}

} // namespace core
