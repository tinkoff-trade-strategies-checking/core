#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "convert/market.hpp"
#include "convert/state.hpp"
#include "core/engine/session_engine.hpp"
#include "handlers.hpp"

namespace worker {

namespace {

void fill(Context &Context, const pb::EstimateOrderRequest &Request,
          pb::EstimateOrderResponse *Out) {
  const pb::MarketSpec &Market = Request.market();
  const core::SessionSpec Spec = toSessionSpec(Market);
  const std::shared_ptr<const core::Timeline> Timeline =
      loadTimeline(Context, Market);

  if (Request.up_to_step() < 0 ||
      Request.up_to_step() >= Timeline->totalSteps()) {
    throw std::invalid_argument("up_to_step вне шкалы времени сессии: " +
                                std::to_string(Request.up_to_step()) +
                                ", шагов " +
                                std::to_string(Timeline->totalSteps()));
  }
  if (!Request.has_order())
    throw std::invalid_argument("не указана заявка для оценки");

  std::vector<core::JournalEntry> Journal;
  Journal.reserve(static_cast<std::size_t>(Request.journal_size()));
  for (const pb::JournalEntry &E : Request.journal())
    Journal.push_back(fromProto(E, Spec));

  const core::OrderSpec Order = orderSpecFromProto(Request.order(), Spec);
  const core::OrderEstimate Estimate =
      core::estimateOrder(Spec, Timeline, Journal, Request.up_to_step(), Order);
  toProto(Estimate, *Timeline, Out);
}

} // namespace

grpc::Status handleEstimateOrder(Context &Context,
                                 const pb::EstimateOrderRequest &Request,
                                 pb::EstimateOrderResponse *Response) {
  try {
    pb::EstimateOrderResponse Result;
    fill(Context, Request, &Result);
    *Response = std::move(Result);
    return grpc::Status::OK;
  } catch (...) {
    return statusFromCurrentException();
  }
}

} // namespace worker
