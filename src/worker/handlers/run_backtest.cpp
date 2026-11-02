#include "handlers.hpp"

namespace worker {

grpc::Status handleRunBacktest(Context&, const pb::RunBacktestRequest&, grpc::ServerWriterInterface<pb::RunBacktestUpdate>*) {
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "RunBacktest ещё не реализован");
}

}  // namespace worker
