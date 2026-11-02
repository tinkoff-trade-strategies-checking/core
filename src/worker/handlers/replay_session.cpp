#include "handlers.hpp"

namespace worker {

grpc::Status handleReplaySession(Context&, const pb::ReplaySessionRequest&, pb::ReplaySessionResponse*) {
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "ReplaySession ещё не реализован");
}

}  // namespace worker
