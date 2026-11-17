#include <stdexcept>
#include <string>
#include <utility>

#include "convert/market.hpp"
#include "handlers.hpp"

namespace worker {

namespace {

void fill(Context &Context, const pb::GetBarsRequest &Request,
          pb::GetBarsResponse *Out) {
  const std::shared_ptr<const core::Timeline> Timeline =
      loadTimeline(Context, Request.market());
  Out->set_total_steps(Timeline->totalSteps());
  Out->set_warmup_steps(Timeline->warmupSteps());
  // Ровно запрошенный диапазон: бэкенд берёт по нему первую цену и график
  // (INV-1). Шаги < 0 - прогрев.
  appendBars(*Timeline, Request.from_step(), Request.to_step(),
             Out->mutable_bars());
}

} // namespace

// Ни одно исключение не выходит наружу (statusFromCurrentException).
grpc::Status handleGetBars(Context &Context, const pb::GetBarsRequest &Request,
                           pb::GetBarsResponse *Response) {
  try {
    pb::GetBarsResponse Result;
    fill(Context, Request, &Result);
    *Response = std::move(Result);
    return grpc::Status::OK;
  } catch (...) {
    return statusFromCurrentException();
  }
}

} // namespace worker
