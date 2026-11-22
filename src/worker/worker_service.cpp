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
WorkerService::ReplaySession(grpc::ServerContext *Context,
                             const pb::ReplaySessionRequest *Request,
                             pb::ReplaySessionResponse *Response) {
  if (Context != nullptr && Context->IsCancelled())
    return grpc::Status::CANCELLED;
  const grpc::Status Status =
      handleReplaySession(HandlerContext, *Request, Response);
  if (Context != nullptr && Context->IsCancelled())
    return grpc::Status::CANCELLED;
  return Status;
}

grpc::Status WorkerService::GetBars(grpc::ServerContext *,
                                    const pb::GetBarsRequest *Request,
                                    pb::GetBarsResponse *Response) {
  return handleGetBars(HandlerContext, *Request, Response);
}

grpc::Status
WorkerService::EstimateOrder(grpc::ServerContext *,
                             const pb::EstimateOrderRequest *Request,
                             pb::EstimateOrderResponse *Response) {
  return handleEstimateOrder(HandlerContext, *Request, Response);
}

grpc::Status
WorkerService::ClassifyPeriods(grpc::ServerContext *,
                               const pb::ClassifyPeriodsRequest *Request,
                               pb::ClassifyPeriodsResponse *Response) {
  return handleClassifyPeriods(HandlerContext, *Request, Response);
}

grpc::Status
WorkerService::CheckStrategy(grpc::ServerContext *Context,
                             const pb::CheckStrategyRequest *Request,
                             pb::CheckStrategyResponse *Response) {
  if (Context != nullptr && Context->IsCancelled())
    return grpc::Status::CANCELLED;
  return handleCheckStrategy(*Request, Response);
}

grpc::Status
WorkerService::RunStrategy(grpc::ServerContext *Context,
                           const pb::RunStrategyRequest *Request,
                           grpc::ServerWriter<pb::RunStrategyUpdate> *Writer) {
  if (Context != nullptr && Context->IsCancelled())
    return grpc::Status::CANCELLED;
  const auto IsCancelled = [Context]() {
    return Context != nullptr && Context->IsCancelled();
  };
  return handleRunStrategy(HandlerContext, *Request, Writer, IsCancelled);
}

grpc::Status
WorkerService::AnalyzeDivergences(grpc::ServerContext *Context,
                                  const pb::DivergenceRequest *Request,
                                  pb::DivergenceResponse *Response) {
  if (Context != nullptr && Context->IsCancelled())
    return grpc::Status::CANCELLED;
  const auto IsCancelled = [Context]() {
    return Context != nullptr && Context->IsCancelled();
  };
  const grpc::Status Status =
      handleAnalyzeDivergences(HandlerContext, *Request, Response, IsCancelled);
  if (Context != nullptr && Context->IsCancelled())
    return grpc::Status::CANCELLED;
  return Status;
}

} // namespace worker
