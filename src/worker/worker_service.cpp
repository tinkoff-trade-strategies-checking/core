#include "worker_service.hpp"

#include <string>

#include "core/version.hpp"

namespace worker {

grpc::Status WorkerService::Ping(grpc::ServerContext *, const pb::PingRequest *,
                                 pb::PingResponse *response) {
  response->set_core_version(std::string(core::version()));
  return grpc::Status::OK;
}

grpc::Status
WorkerService::CheckStrategy(grpc::ServerContext *,
                             const pb::CheckStrategyRequest *request,
                             pb::CheckStrategyResponse *response) {
  return handleCheckStrategy(context_, *request, response);
}

grpc::Status
WorkerService::ReplaySession(grpc::ServerContext *,
                             const pb::ReplaySessionRequest *request,
                             pb::ReplaySessionResponse *response) {
  return handleReplaySession(context_, *request, response);
}

grpc::Status WorkerService::GetBars(grpc::ServerContext *,
                                    const pb::GetBarsRequest *request,
                                    pb::GetBarsResponse *response) {
  return handleGetBars(context_, *request, response);
}

grpc::Status
WorkerService::RunBacktest(grpc::ServerContext *,
                           const pb::RunBacktestRequest *request,
                           grpc::ServerWriter<pb::RunBacktestUpdate> *writer) {
  return handleRunBacktest(context_, *request, writer);
}

grpc::Status WorkerService::RunWalkForward(
    grpc::ServerContext *, const pb::WalkForwardRequest *request,
    grpc::ServerWriter<pb::WalkForwardUpdate> *writer) {
  return handleRunWalkForward(context_, *request, writer);
}

grpc::Status
WorkerService::AnalyzeDivergences(grpc::ServerContext *,
                                  const pb::DivergenceRequest *request,
                                  pb::DivergenceResponse *response) {
  return handleAnalyzeDivergences(context_, *request, response);
}

} // namespace worker
