#include "handlers.hpp"

namespace worker {

grpc::Status handleCheckStrategy(Context&, const pb::CheckStrategyRequest&, pb::CheckStrategyResponse*) {
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "CheckStrategy ещё не реализован");
}

}  // namespace worker
