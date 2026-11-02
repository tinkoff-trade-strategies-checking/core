#include <stdexcept>
#include <string>
#include <utility>

#include "convert/market.hpp"
#include "handlers.hpp"

namespace worker {

namespace {

void fill(Context& context, const pb::GetBarsRequest& request, pb::GetBarsResponse* out) {
  const std::shared_ptr<const core::Timeline> timeline = loadStressedTimeline(context, request.market(), request.stress(), request.has_stress());
  const std::int64_t total = timeline->totalSteps();
  if (request.from_step() < 0 || request.to_step() >= total || request.from_step() > request.to_step()) {
    throw std::invalid_argument("диапазон шагов [" + std::to_string(request.from_step()) + "; " +
                                std::to_string(request.to_step()) + "] вне шкалы времени, шагов " +
                                std::to_string(total));
  }
  out->set_total_steps(total);
  // Ровно запрошенный диапазон: бэкенд берёт по нему первую цену и график (INV-1).
  for (std::int64_t step = request.from_step(); step <= request.to_step(); ++step) {
    for (const core::StepBar& b : timeline->barsAt(step)) toProto(b, *timeline, out->add_bars());
  }
}

}  // namespace

// Ни одно исключение не выходит наружу (statusFromCurrentException).
grpc::Status handleGetBars(Context& context, const pb::GetBarsRequest& request, pb::GetBarsResponse* response) {
  try {
    pb::GetBarsResponse result;
    fill(context, request, &result);
    *response = std::move(result);
    return grpc::Status::OK;
  } catch (...) {
    return statusFromCurrentException();
  }
}

}  // namespace worker
