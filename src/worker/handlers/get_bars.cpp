#include "handlers.hpp"

namespace worker {

grpc::Status handleGetBars(Context&, const pb::GetBarsRequest&, pb::GetBarsResponse*) {
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "GetBars ещё не реализован");
}

}  // namespace worker
