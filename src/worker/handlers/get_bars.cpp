#include <stdexcept>
#include <string>
#include <utility>

#include "convert/market.hpp"
#include "handlers.hpp"

namespace worker {

namespace {

void fill(Context &context, const pb::GetBarsRequest &request,
          pb::GetBarsResponse *out) {
  const std::shared_ptr<const core::Timeline> timeline = loadStressedTimeline(
      context, request.market(), request.stress(), request.has_stress());
  out->set_total_steps(timeline->totalSteps());
  out->set_warmup_steps(timeline->warmupSteps());
  // Ровно запрошенный диапазон: бэкенд берёт по нему первую цену и график
  // (INV-1). Шаги < 0 - прогрев.
  appendBars(*timeline, request.from_step(), request.to_step(),
             out->mutable_bars());
}

} // namespace

// Ни одно исключение не выходит наружу (statusFromCurrentException).
grpc::Status handleGetBars(Context &context, const pb::GetBarsRequest &request,
                           pb::GetBarsResponse *response) {
  try {
    pb::GetBarsResponse result;
    fill(context, request, &result);
    *response = std::move(result);
    return grpc::Status::OK;
  } catch (...) {
    return statusFromCurrentException();
  }
}

} // namespace worker
