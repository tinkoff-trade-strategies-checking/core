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

} // namespace worker
