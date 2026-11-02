#include "handlers.hpp"

namespace worker {

grpc::Status handleRunWalkForward(Context&, const pb::WalkForwardRequest&, grpc::ServerWriterInterface<pb::WalkForwardUpdate>*) {
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "RunWalkForward ещё не реализован");
}

}  // namespace worker
