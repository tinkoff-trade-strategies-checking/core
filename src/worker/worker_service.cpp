#include "worker_service.hpp"

#include <string>

#include "core/version.hpp"

namespace worker {

grpc::Status WorkerService::Ping(grpc::ServerContext *, const pb::PingRequest *,
                                 pb::PingResponse *Response) {
  Response->set_core_version(std::string(core::version()));
  return grpc::Status::OK;
}

grpc::Status
WorkerService::CheckStrategy(grpc::ServerContext *,
                             const pb::CheckStrategyRequest *Request,
                             pb::CheckStrategyResponse *Response) {
  return handleCheckStrategy(HandlerContext, *Request, Response);
}

grpc::Status
WorkerService::ReplaySession(grpc::ServerContext *,
                             const pb::ReplaySessionRequest *Request,
                             pb::ReplaySessionResponse *Response) {
  return handleReplaySession(HandlerContext, *Request, Response);
}

grpc::Status WorkerService::GetBars(grpc::ServerContext *,
                                    const pb::GetBarsRequest *Request,
                                    pb::GetBarsResponse *Response) {
  return handleGetBars(HandlerContext, *Request, Response);
}

grpc::Status
WorkerService::RunBacktest(grpc::ServerContext *,
                           const pb::RunBacktestRequest *Request,
                           grpc::ServerWriter<pb::RunBacktestUpdate> *Writer) {
  return handleRunBacktest(HandlerContext, *Request, Writer);
}

grpc::Status WorkerService::RunWalkForward(
    grpc::ServerContext *, const pb::WalkForwardRequest *Request,
    grpc::ServerWriter<pb::WalkForwardUpdate> *Writer) {
  return handleRunWalkForward(HandlerContext, *Request, Writer);
}

grpc::Status
WorkerService::AnalyzeDivergences(grpc::ServerContext *,
                                  const pb::DivergenceRequest *Request,
                                  pb::DivergenceResponse *Response) {
  return handleAnalyzeDivergences(HandlerContext, *Request, Response);
}

} // namespace worker
