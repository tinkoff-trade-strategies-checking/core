#include "convert/state.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "convert/market.hpp"
#include "core/blind/blind.hpp"
#include "core/money/proto_conv.hpp"
#include "core/report/derived.hpp"

namespace worker {

namespace {

int instrumentIndex(const std::string &Key, const core::SessionSpec &Spec) {
  for (std::size_t I = 0; I < Spec.Instruments.size(); ++I) {
    if (Spec.Instruments[I].Key == Key)
      return static_cast<int>(I);
  }
  throw std::invalid_argument("в журнале неизвестная бумага: " + Key);
}

const std::string &keyOf(const core::Timeline &Timeline, int Instrument) {
  return Timeline.instruments().at(static_cast<std::size_t>(Instrument)).Key;
}

core::Side sideFromProto(pb::Side Side) {
  switch (Side) {
  case pb::BUY:
    return core::Side::Buy;
  case pb::SELL:
    return core::Side::Sell;
  default:
    throw std::invalid_argument("не указана сторона заявки");
  }
}

core::OrderType typeFromProto(pb::OrderType Type) {
  switch (Type) {
  case pb::MARKET:
    return core::OrderType::Market;
  case pb::LIMIT:
    return core::OrderType::Limit;
  case pb::STOP:
    return core::OrderType::Stop;
  case pb::STOP_LIMIT:
    return core::OrderType::StopLimit;
  case pb::TAKE_PROFIT:
    return core::OrderType::TakeProfit;
  case pb::TRAILING_STOP:
    return core::OrderType::TrailingStop;
  default:
    throw std::invalid_argument("не указан тип заявки");
  }
}

core::TimeInForce tifFromProto(pb::TimeInForce Tif) {
  switch (Tif) {
  case pb::DAY:
    return core::TimeInForce::Day;
  case pb::GTC:
    return core::TimeInForce::Gtc;
  default:
    throw std::invalid_argument("не указан срок действия заявки");
  }
}

core::OrderSpec specFromProto(const pb::OrderSpec &Spec,
                              const core::SessionSpec &Session) {
  core::OrderSpec Out;
  Out.Instrument = instrumentIndex(Spec.instrument_key(), Session);
  Out.OrderSide = sideFromProto(Spec.side());
  Out.Type = typeFromProto(Spec.type());
  Out.Lots = Spec.lots();
  if (Spec.has_limit_price())
    Out.LimitPrice = core::fromProto(Spec.limit_price());
  if (Spec.has_stop_price())
    Out.StopPrice = core::fromProto(Spec.stop_price());
  if (Spec.has_trailing_percent())
    Out.TrailingPercent = core::fromProto(Spec.trailing_percent());
  Out.Tif = tifFromProto(Spec.time_in_force());
  Out.IsBlindPrice = Spec.is_blind_price();
  return Out;
}

core::PlaceCmd placeFromProto(const pb::PlaceOrder &Place,
                              const core::SessionSpec &Session) {
  core::PlaceCmd Cmd;
  Cmd.Order = specFromProto(Place.order(), Session);
  if (Place.has_oco_with() && Place.oco_with().lots() != 0)
    Cmd.OcoWith = specFromProto(Place.oco_with(), Session);
  return Cmd;
}

void specToProto(const core::OrderSpec &Spec, const core::Timeline &Timeline,
                 pb::OrderSpec *Out) {
  Out->set_instrument_key(keyOf(Timeline, Spec.Instrument));
  Out->set_side(toProto(Spec.OrderSide));
  Out->set_type(toProto(Spec.Type));
  Out->set_lots(Spec.Lots);
  if (Spec.LimitPrice)
    core::toProto(*Spec.LimitPrice, Out->mutable_limit_price());
  if (Spec.StopPrice)
    core::toProto(*Spec.StopPrice, Out->mutable_stop_price());
  if (Spec.TrailingPercent)
    core::toProto(*Spec.TrailingPercent, Out->mutable_trailing_percent());
  Out->set_time_in_force(toProto(Spec.Tif));
  Out->set_is_blind_price(Spec.IsBlindPrice);
}

} // namespace

core::JournalEntry fromProto(const pb::JournalEntry &Entry,
                             const core::SessionSpec &Spec) {
  core::JournalEntry Out;
  Out.Seq = Entry.seq();
  Out.Step = Entry.step();
  switch (Entry.command_case()) {
  case pb::JournalEntry::kPlace:
    Out.Command = placeFromProto(Entry.place(), Spec);
    break;
  case pb::JournalEntry::kCancel: {
    const pb::Reason Reason = Entry.cancel().reason();
    Out.Command = core::CancelCmd{Entry.cancel().order_id(),
                                  Reason == pb::REASON_UNSPECIFIED
                                      ? core::Reason::CancelledByUser
                                      : reasonFromProto(Reason)};
    break;
  }
  case pb::JournalEntry::kReplace:
    Out.Command =
        core::ReplaceCmd{Entry.replace().order_id(),
                         placeFromProto(Entry.replace().replacement(), Spec)};
    break;
  case pb::JournalEntry::COMMAND_NOT_SET:
    throw std::invalid_argument("в записи журнала нет команды, seq " +
                                std::to_string(Entry.seq()));
  }
  return Out;
}

namespace {

void placeToProto(const core::PlaceCmd &Place, const core::Timeline &Timeline,
                  pb::PlaceOrder *Out) {
  specToProto(Place.Order, Timeline, Out->mutable_order());
  if (Place.OcoWith)
    specToProto(*Place.OcoWith, Timeline, Out->mutable_oco_with());
}

} // namespace

void toProto(const core::JournalEntry &Entry, const core::Timeline &Timeline,
             pb::JournalEntry *Out) {
  Out->set_seq(Entry.Seq);
  Out->set_step(Entry.Step);
  if (const auto *Place = std::get_if<core::PlaceCmd>(&Entry.Command)) {
    placeToProto(*Place, Timeline, Out->mutable_place());
  } else if (const auto *Cancel =
                 std::get_if<core::CancelCmd>(&Entry.Command)) {
    Out->mutable_cancel()->set_order_id(Cancel->OrderId);
    Out->mutable_cancel()->set_reason(toProto(Cancel->ReasonCode));
  } else {
    const auto &Replace = std::get<core::ReplaceCmd>(Entry.Command);
    Out->mutable_replace()->set_order_id(Replace.OrderId);
    placeToProto(Replace.Replacement, Timeline,
                 Out->mutable_replace()->mutable_replacement());
  }
}

core::Reason reasonFromProto(pb::Reason Reason) {
  for (int I = static_cast<int>(core::Reason::None);
       I <= static_cast<int>(core::Reason::DayExpired); ++I) {
    const auto Candidate = static_cast<core::Reason>(I);
    if (toProto(Candidate) == Reason)
      return Candidate;
  }
  throw std::invalid_argument("неизвестная причина: " +
                              std::to_string(static_cast<int>(Reason)));
}

void toProto(const core::Order &Order, const core::Timeline &Timeline,
             pb::Order *Out) {
  Out->set_id(Order.Id);
  specToProto(Order.Spec, Timeline, Out->mutable_spec());
  Out->set_placed_step(Order.PlacedStep);
  Out->set_status(toProto(Order.Status));
  Out->set_reason(toProto(Order.ReasonCode));
  Out->set_invalid_field(Order.InvalidField);
  Out->set_filled_lots(Order.FilledLots);
  Out->set_oco_sibling_id(Order.OcoSiblingId);
  if (Order.TrailingStopLevel)
    core::toProto(*Order.TrailingStopLevel, Out->mutable_trailing_stop_level());
  Out->set_closed_step(Order.ClosedStep);
}

void toProto(const core::Fill &Fill, const core::Timeline &Timeline,
             const core::Decimal &InitialCash, pb::Fill *Out) {
  Out->set_order_id(Fill.OrderId);
  Out->set_step(Fill.Step);
  Out->set_instrument_key(keyOf(Timeline, Fill.Instrument));
  Out->set_side(toProto(Fill.OrderSide));
  Out->set_lots(Fill.Lots);
  core::toProto(Fill.Price, Out->mutable_price());
  core::toProto(Fill.BasePrice, Out->mutable_base_price());
  core::toProto(Fill.Value, Out->mutable_value());
  core::toProto(Fill.Commission, Out->mutable_commission());
  core::toProto(Fill.Fee, Out->mutable_exchange_fee());
  Out->set_stop_and_take_same_bar(Fill.StopAndTakeSameBar);
  core::toProto(Fill.SpreadCost, Out->mutable_spread_cost());
  core::toProto(Fill.SlippageCost, Out->mutable_slippage_cost());
  Out->set_seq(Fill.Seq);
  core::toProto(Fill.cost(), Out->mutable_cost());
  core::toProto(core::toPercentOfCapital(Fill.cost(), InitialCash),
                Out->mutable_cost_pct());
  core::toProto(core::toPercentOfCapital(Fill.Value, InitialCash),
                Out->mutable_amount_pct());
}

void toProto(const core::PortfolioSnapshot &Snapshot,
             const core::Timeline &Timeline, const core::Decimal &InitialCash,
             const core::Decimal &CumulativeCosts, pb::Portfolio *Out) {
  core::toProto(Snapshot.Cash, Out->mutable_cash());
  core::toProto(Snapshot.Equity, Out->mutable_equity());
  const core::PortfolioPcts Pcts =
      core::portfolioPcts(Snapshot, InitialCash, CumulativeCosts);
  core::toProto(Pcts.InvestedPct, Out->mutable_invested_pct());
  core::toProto(Pcts.CostsPct, Out->mutable_costs_pct());
  core::toProto(Pcts.PnlPct, Out->mutable_pnl_pct());
  for (std::size_t I = 0; I < Snapshot.Lots.size(); ++I) {
    if (Snapshot.Lots[I] == 0)
      continue;
    pb::Position *P = Out->add_positions();
    P->set_instrument_key(keyOf(Timeline, static_cast<int>(I)));
    P->set_lots(Snapshot.Lots[I]);
    core::toProto(Snapshot.AvgPrice[I], P->mutable_avg_price());
    core::toProto(Snapshot.MarketValue[I], P->mutable_market_value());
    core::toProto(
        core::positionSharePct(Snapshot.MarketValue[I], Snapshot.Equity),
        P->mutable_share_pct());
    core::toProto(core::positionPnlPct(Snapshot.MarketValue[I],
                                       Snapshot.AvgPrice[I], Snapshot.Lots[I],
                                       Timeline.instruments()[I].LotSize),
                  P->mutable_pnl_pct());
  }
}

void toProto(const core::CommandResult &Result, pb::CommandResult *Out) {
  Out->set_seq(Result.Seq);
  Out->set_accepted(Result.Accepted);
  Out->set_reason(toProto(Result.ReasonCode));
  Out->set_invalid_field(Result.Field);
  for (const std::int64_t Id : Result.OrderIds)
    Out->add_order_ids(Id);
}

void toProto(const core::StepEvents &Events, const core::Timeline &Timeline,
             const core::Decimal &InitialCash,
             const std::vector<core::Fill> &AllFills, pb::StepEvents *Out) {
  Out->set_step(Events.Step);
  for (const core::StepBar &B : Events.Bars)
    toProto(B, Timeline, Out->add_bars());
  for (const core::Fill &F : Events.Fills)
    toProto(F, Timeline, InitialCash, Out->add_fills());
  for (const core::Order &O : Events.ChangedOrders)
    toProto(O, Timeline, Out->add_changed_orders());
  toProto(Events.Portfolio, Timeline, InitialCash,
          core::cumulativeCosts(AllFills, Events.Step),
          Out->mutable_portfolio());
}

pb::SessionStatus toProto(core::SessionStatus Status) {
  switch (Status) {
  case core::SessionStatus::Running:
    return pb::RUNNING;
  case core::SessionStatus::FinishedEnd:
    return pb::FINISHED_END;
  case core::SessionStatus::Bankrupt:
    return pb::BANKRUPT;
  }
  return pb::SESSION_STATUS_UNSPECIFIED;
}

pb::Side toProto(core::Side Side) {
  return Side == core::Side::Buy ? pb::BUY : pb::SELL;
}

pb::OrderType toProto(core::OrderType Type) {
  switch (Type) {
  case core::OrderType::Market:
    return pb::MARKET;
  case core::OrderType::Limit:
    return pb::LIMIT;
  case core::OrderType::Stop:
    return pb::STOP;
  case core::OrderType::StopLimit:
    return pb::STOP_LIMIT;
  case core::OrderType::TakeProfit:
    return pb::TAKE_PROFIT;
  case core::OrderType::TrailingStop:
    return pb::TRAILING_STOP;
  }
  return pb::ORDER_TYPE_UNSPECIFIED;
}

pb::TimeInForce toProto(core::TimeInForce Tif) {
  return Tif == core::TimeInForce::Day ? pb::DAY : pb::GTC;
}

pb::OrderStatus toProto(core::OrderStatus Status) {
  switch (Status) {
  case core::OrderStatus::Active:
    return pb::ACTIVE;
  case core::OrderStatus::PartiallyFilled:
    return pb::PARTIALLY_FILLED;
  case core::OrderStatus::Filled:
    return pb::FILLED;
  case core::OrderStatus::Cancelled:
    return pb::CANCELLED;
  case core::OrderStatus::Expired:
    return pb::EXPIRED;
  case core::OrderStatus::Rejected:
    return pb::REJECTED;
  }
  return pb::ORDER_STATUS_UNSPECIFIED;
}

pb::Reason toProto(core::Reason Reason) {
  switch (Reason) {
  case core::Reason::None:
    return pb::REASON_UNSPECIFIED;
  case core::Reason::InvalidParams:
    return pb::INVALID_PARAMS;
  case core::Reason::InsufficientCash:
    return pb::INSUFFICIENT_CASH;
  case core::Reason::InsufficientPosition:
    return pb::INSUFFICIENT_POSITION;
  case core::Reason::SessionNotActive:
    return pb::SESSION_NOT_ACTIVE;
  case core::Reason::OrderNotActive:
    return pb::ORDER_NOT_ACTIVE;
  case core::Reason::LiquidityLimit:
    return pb::LIQUIDITY_LIMIT;
  case core::Reason::OcoSiblingFilled:
    return pb::OCO_SIBLING_FILLED;
  case core::Reason::Replaced:
    return pb::REPLACED;
  case core::Reason::SessionEnd:
    return pb::SESSION_END;
  case core::Reason::CancelledByUser:
    return pb::CANCELLED_BY_USER;
  case core::Reason::DayExpired:
    return pb::DAY_EXPIRED;
  }
  return pb::REASON_UNSPECIFIED;
}

core::OrderSpec orderSpecFromProto(const pb::OrderSpec &Spec,
                                   const core::SessionSpec &Session) {
  return specFromProto(Spec, Session);
}

void toProto(const core::OrderEstimate &Estimate,
             const core::Timeline &Timeline, pb::EstimateOrderResponse *Out) {
  Out->set_can_place(Estimate.Check.Ok);
  Out->set_reason(toProto(Estimate.Check.ReasonCode));
  Out->set_invalid_field(Estimate.Check.Field);
  core::toProto(Estimate.ReferencePrice, Out->mutable_reference_price());
  core::toProto(Estimate.Value, Out->mutable_amount());
  core::toProto(Estimate.Commission, Out->mutable_commission());
  core::toProto(Estimate.Fee, Out->mutable_exchange_fee());
  core::toProto(Estimate.RequiredCash, Out->mutable_required_cash());
  core::toProto(Estimate.ValuePct, Out->mutable_amount_pct());
  core::toProto(Estimate.CommissionPct, Out->mutable_commission_pct());
  core::toProto(Estimate.FeePct, Out->mutable_exchange_fee_pct());
  core::toProto(Estimate.RequiredCashPct, Out->mutable_required_cash_pct());
  if (Estimate.ResolvedOrder)
    specToProto(*Estimate.ResolvedOrder, Timeline,
                Out->mutable_resolved_order());
}

} // namespace worker
