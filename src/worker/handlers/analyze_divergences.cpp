#include "handlers.hpp"

namespace worker {

grpc::Status handleAnalyzeDivergences(Context&, const pb::DivergenceRequest&, pb::DivergenceResponse*) {
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "AnalyzeDivergences ещё не реализован");
}

}  // namespace worker
