#include <cstddef>

#include "core/exec/execution.hpp"

namespace core {

namespace {

Validation invalid(const char *field) {
  return Validation{false, Reason::InvalidParams, field};
}

bool positive(const std::optional<Decimal> &v) {
  return v.has_value() && v->sign() > 0;
}

} // namespace

Validation ExecutionEngine::validate(const OrderSpec &spec,
                                     const Portfolio &portfolio,
                                     const Timeline &timeline,
                                     std::int64_t step) const {
  if (spec.instrument < 0 ||
      static_cast<std::size_t>(spec.instrument) >= instruments_.size()) {
    return invalid("instrument");
  }
  if (spec.lots < 1)
    return invalid("lots");

  const bool buy = spec.side == Side::Buy;
  const OrderType type = spec.type;
  if ((type == OrderType::TakeProfit || type == OrderType::TrailingStop) && buy)
    return invalid("side");

  const bool needsLimit = type == OrderType::Limit ||
                          type == OrderType::StopLimit ||
                          type == OrderType::TakeProfit;
  if (needsLimit && !positive(spec.limitPrice))
    return invalid("limitPrice");
  const bool needsStop =
      type == OrderType::Stop || type == OrderType::StopLimit;
  if (needsStop && !positive(spec.stopPrice))
    return invalid("stopPrice");
  if (type == OrderType::TrailingStop) {
    const bool ok = spec.trailingPercent && spec.trailingPercent->sign() > 0 &&
                    *spec.trailingPercent <= Decimal::fromUnits(50);
    if (!ok)
      return invalid("trailingPercent");
  }

  // Цена для проверок - close последнего бара бумаги не позже шага подачи
  // (`decisions.md`, «Заявки»).
  const StepBar *last = timeline.lastBarOf(spec.instrument, step);
  if (last == nullptr)
    return invalid("instrument"); // бумага ещё не торговалась: цены нет
  const Decimal &close = last->close;

  if (needsStop) {
    const bool rightSide =
        buy ? *spec.stopPrice > close : *spec.stopPrice < close;
    if (!rightSide)
      return invalid("stopPrice");
  }
  if (type == OrderType::TakeProfit && !(*spec.limitPrice > close))
    return invalid("limitPrice");

  if (buy) {
    const Instrument &inst =
        instruments_[static_cast<std::size_t>(spec.instrument)];
    const Decimal factor =
        Decimal::one() + costs_.commissionRate + costs_.exchangeFeeRate;
    try {
      // Округление вверх: проверка не пропускает заявку, на которую не хватает
      // копейки.
      const Decimal need =
          close.times(spec.lots)
              .times(inst.lotSize)
              .mul(factor, Decimal::kScale, RoundingMode::CEILING);
      if (need > portfolio.cash())
        return Validation{false, Reason::InsufficientCash, ""};
    } catch (const DecimalOverflow &) {
      return Validation{
          false, Reason::InsufficientCash,
          ""}; // сумма не помещается в Decimal - денег точно не хватает
    }
  } else if (spec.lots > portfolio.lots(spec.instrument)) {
    return Validation{false, Reason::InsufficientPosition, ""};
  }
  return Validation{};
}

} // namespace core
