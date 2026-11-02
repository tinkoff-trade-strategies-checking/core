#include "convert/state.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "convert/market.hpp"
#include "core/money/proto_conv.hpp"

namespace worker {

namespace {

int instrumentIndex(const std::string& key, const core::SessionSpec& spec) {
  for (std::size_t i = 0; i < spec.instruments.size(); ++i) {
    if (spec.instruments[i].key == key) return static_cast<int>(i);
  }
  throw std::invalid_argument("в журнале неизвестная бумага: " + key);
}

const std::string& keyOf(const core::Timeline& timeline, int instrument) {
  return timeline.instruments().at(static_cast<std::size_t>(instrument)).key;
}

core::Side sideFromProto(pb::Side side) {
  switch (side) {
    case pb::BUY:
      return core::Side::Buy;
    case pb::SELL:
      return core::Side::Sell;
    default:
      throw std::invalid_argument("не указана сторона заявки");
  }
}

core::OrderType typeFromProto(pb::OrderType type) {
  switch (type) {
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

core::TimeInForce tifFromProto(pb::TimeInForce tif) {
  switch (tif) {
    case pb::DAY:
      return core::TimeInForce::Day;
    case pb::GTC:
      return core::TimeInForce::Gtc;
    default:
      throw std::invalid_argument("не указан срок действия заявки");
  }
}

core::OrderSpec specFromProto(const pb::OrderSpec& spec, const core::SessionSpec& session) {
  core::OrderSpec out;
  out.instrument = instrumentIndex(spec.instrument_key(), session);
  out.side = sideFromProto(spec.side());
  out.type = typeFromProto(spec.type());
  out.lots = spec.lots();
  if (spec.has_limit_price()) out.limitPrice = core::fromProto(spec.limit_price());
  if (spec.has_stop_price()) out.stopPrice = core::fromProto(spec.stop_price());
  if (spec.has_trailing_percent()) out.trailingPercent = core::fromProto(spec.trailing_percent());
  out.tif = tifFromProto(spec.time_in_force());
  return out;
}

core::PlaceCmd placeFromProto(const pb::PlaceOrder& place, const core::SessionSpec& session) {
  core::PlaceCmd cmd;
  cmd.order = specFromProto(place.order(), session);
  if (place.has_oco_with() && place.oco_with().lots() != 0) cmd.ocoWith = specFromProto(place.oco_with(), session);
  return cmd;
}

void specToProto(const core::OrderSpec& spec, const core::Timeline& timeline, pb::OrderSpec* out) {
  out->set_instrument_key(keyOf(timeline, spec.instrument));
  out->set_side(toProto(spec.side));
  out->set_type(toProto(spec.type));
  out->set_lots(spec.lots);
  if (spec.limitPrice) core::toProto(*spec.limitPrice, out->mutable_limit_price());
  if (spec.stopPrice) core::toProto(*spec.stopPrice, out->mutable_stop_price());
  if (spec.trailingPercent) core::toProto(*spec.trailingPercent, out->mutable_trailing_percent());
  out->set_time_in_force(toProto(spec.tif));
}

}  // namespace

core::JournalEntry fromProto(const pb::JournalEntry& entry, const core::SessionSpec& spec) {
  core::JournalEntry out;
  out.seq = entry.seq();
  out.step = entry.step();
  switch (entry.command_case()) {
    case pb::JournalEntry::kPlace:
      out.command = placeFromProto(entry.place(), spec);
      break;
    case pb::JournalEntry::kCancel:
      out.command = core::CancelCmd{entry.cancel().order_id(), core::Reason::CancelledByUser};
      break;
    case pb::JournalEntry::kReplace:
      out.command = core::ReplaceCmd{entry.replace().order_id(), placeFromProto(entry.replace().replacement(), spec)};
      break;
    case pb::JournalEntry::COMMAND_NOT_SET:
      throw std::invalid_argument("в записи журнала нет команды, seq " + std::to_string(entry.seq()));
  }
  return out;
}

void toProto(const core::Order& order, const core::Timeline& timeline, pb::Order* out) {
  out->set_id(order.id);
  specToProto(order.spec, timeline, out->mutable_spec());
  out->set_placed_step(order.placedStep);
  out->set_status(toProto(order.status));
  out->set_reason(toProto(order.reason));
  out->set_invalid_field(order.invalidField);
  out->set_filled_lots(order.filledLots);
  out->set_oco_sibling_id(order.ocoSiblingId);
  out->set_rule_name(order.ruleName);
  out->set_because(order.because);
  if (order.trailingStopLevel) core::toProto(*order.trailingStopLevel, out->mutable_trailing_stop_level());
  out->set_closed_step(order.closedStep);
}

void toProto(const core::Fill& fill, const core::Timeline& timeline, pb::Fill* out) {
  out->set_order_id(fill.orderId);
  out->set_step(fill.step);
  out->set_instrument_key(keyOf(timeline, fill.instrument));
  out->set_side(toProto(fill.side));
  out->set_lots(fill.lots);
  core::toProto(fill.price, out->mutable_price());
  core::toProto(fill.basePrice, out->mutable_base_price());
  core::toProto(fill.value, out->mutable_value());
  core::toProto(fill.commission, out->mutable_commission());
  core::toProto(fill.fee, out->mutable_exchange_fee());
  out->set_stop_and_take_same_bar(fill.stopAndTakeSameBar);
  core::toProto(fill.spreadCost, out->mutable_spread_cost());
  core::toProto(fill.slippageCost, out->mutable_slippage_cost());
}

void toProto(const core::PortfolioSnapshot& snapshot, const core::Timeline& timeline, pb::Portfolio* out) {
  core::toProto(snapshot.cash, out->mutable_cash());
  core::toProto(snapshot.equity, out->mutable_equity());
  for (std::size_t i = 0; i < snapshot.lots.size(); ++i) {
    if (snapshot.lots[i] == 0) continue;
    pb::Position* p = out->add_positions();
    p->set_instrument_key(keyOf(timeline, static_cast<int>(i)));
    p->set_lots(snapshot.lots[i]);
    core::toProto(snapshot.avgPrice[i], p->mutable_avg_price());
    core::toProto(snapshot.marketValue[i], p->mutable_market_value());
  }
}

void toProto(const core::CommandResult& result, pb::CommandResult* out) {
  out->set_seq(result.seq);
  out->set_accepted(result.accepted);
  out->set_reason(toProto(result.reason));
  out->set_invalid_field(result.field);
  for (const std::int64_t id : result.orderIds) out->add_order_ids(id);
}

void toProto(const core::StepEvents& events, const core::Timeline& timeline, pb::StepEvents* out) {
  out->set_step(events.step);
  for (const core::StepBar& b : events.bars) toProto(b, timeline, out->add_bars());
  for (const core::Fill& f : events.fills) toProto(f, timeline, out->add_fills());
  for (const core::Order& o : events.changedOrders) toProto(o, timeline, out->add_changed_orders());
  toProto(events.portfolio, timeline, out->mutable_portfolio());
}

pb::SessionStatus toProto(core::SessionStatus status) {
  switch (status) {
    case core::SessionStatus::Running:
      return pb::RUNNING;
    case core::SessionStatus::FinishedEnd:
      return pb::FINISHED_END;
    case core::SessionStatus::Bankrupt:
      return pb::BANKRUPT;
  }
  return pb::SESSION_STATUS_UNSPECIFIED;
}

pb::Side toProto(core::Side side) { return side == core::Side::Buy ? pb::BUY : pb::SELL; }

pb::OrderType toProto(core::OrderType type) {
  switch (type) {
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

pb::TimeInForce toProto(core::TimeInForce tif) { return tif == core::TimeInForce::Day ? pb::DAY : pb::GTC; }

pb::OrderStatus toProto(core::OrderStatus status) {
  switch (status) {
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

pb::Reason toProto(core::Reason reason) {
  switch (reason) {
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
    case core::Reason::PositionClosed:
      return pb::POSITION_CLOSED;
    case core::Reason::Replaced:
      return pb::REPLACED;
    case core::Reason::SessionEnd:
      return pb::SESSION_END;
    case core::Reason::CancelledByUser:
      return pb::CANCELLED_BY_USER;
    case core::Reason::RuleReplaced:
      return pb::RULE_REPLACED;
    case core::Reason::DayExpired:
      return pb::DAY_EXPIRED;
  }
  return pb::REASON_UNSPECIFIED;
}

}  // namespace worker
