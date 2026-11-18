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
  const ExecutionEngine Exec(Spec.Instruments, Spec.Costs);
  if (Result.Status != SessionStatus::Running) {
    Out.Check = Validation{false, Reason::SessionNotActive, ""};
  } else {
    Out.Check = Exec.validate(Order, Result.Engine.portfolio(), *TimelineRef,
                              Result.LastStep);
  }

  if (Order.Instrument >= 0 &&
      static_cast<std::size_t>(Order.Instrument) < Spec.Instruments.size() &&
      Order.Lots >= 1) {
    const StepBar *Last =
        TimelineRef->lastBarOf(Order.Instrument, Result.LastStep);
    if (Last != nullptr) {
      const std::int64_t LotSize =
          Spec.Instruments[static_cast<std::size_t>(Order.Instrument)].LotSize;
      Out.ReferencePrice = Last->Close;
      try {
        const Amounts A = amounts(Last->Close, Order.Lots, LotSize, Spec.Costs);
        Out.Value = A.Value;
        Out.Commission = A.Commission;
        Out.Fee = A.Fee;
        Out.RequiredCash =
            requiredCashForOrder(Last->Close, Order.Lots, LotSize, Spec.Costs)
                .round(2, RoundingMode::CEILING);
        Out.ValuePct = toPercentOfCapital(Out.Value, Spec.InitialCash);
        Out.CommissionPct =
            toPercentOfCapital(Out.Commission, Spec.InitialCash);
        Out.FeePct = toPercentOfCapital(Out.Fee, Spec.InitialCash);
        Out.RequiredCashPct =
            toPercentOfCapital(Out.RequiredCash, Spec.InitialCash);
      } catch (const DecimalOverflow &) {
        throw std::invalid_argument(
            "order.lots: переполнение при расчёте суммы заявки");
      }
    }
  }
  return Out;
}

} // namespace core
