#include "worker_service.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>

#include "core/version.hpp"

namespace worker {

namespace {

bool deadlineReached(std::chrono::system_clock::time_point Deadline) {
  return Deadline != std::chrono::system_clock::time_point::max() &&
         std::chrono::system_clock::now() >= Deadline;
}

std::optional<grpc::Status>
contextEndStatus(const grpc::ServerContext *Context,
                 std::chrono::system_clock::time_point Deadline) {
  if (Context == nullptr || !Context->IsCancelled()) {
    return std::nullopt;
  }
  if (deadlineReached(Deadline)) {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                        kTimeBudgetExceededMessage);
  }
  return grpc::Status::CANCELLED;
}

std::function<bool()>
makeClientCancelCheck(const grpc::ServerContext *Context,
                      std::chrono::system_clock::time_point Deadline) {
  return [Context, Deadline]() {
    return Context != nullptr && Context->IsCancelled() &&
           !deadlineReached(Deadline);
  };
}

} // namespace

std::chrono::steady_clock::duration
computeTimeBudget(std::chrono::system_clock::time_point Deadline) {
  if (Deadline == std::chrono::system_clock::time_point::max()) {
    return kMaxRunTimeBudget;
  }
  const auto Now = std::chrono::system_clock::now();
  if (Deadline <= Now) {
    return std::chrono::steady_clock::duration::zero();
  }
  const auto Remaining = Deadline - Now;
  const auto MaxBudget =
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          kMaxRunTimeBudget);
  if (Remaining >= MaxBudget) {
    return kMaxRunTimeBudget;
  }
  return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      Remaining);
}

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
  const auto Deadline = (Context != nullptr)
                            ? Context->deadline()
                            : std::chrono::system_clock::time_point::max();
  if (const auto EndStatus = contextEndStatus(Context, Deadline)) {
    return *EndStatus;
  }
  return handleRunStrategy(HandlerContext, *Request, Writer,
                           makeClientCancelCheck(Context, Deadline),
                           computeTimeBudget(Deadline));
}

grpc::Status
WorkerService::AnalyzeDivergences(grpc::ServerContext *Context,
                                  const pb::DivergenceRequest *Request,
                                  pb::DivergenceResponse *Response) {
  const auto Deadline = (Context != nullptr)
                            ? Context->deadline()
                            : std::chrono::system_clock::time_point::max();
  if (const auto EndStatus = contextEndStatus(Context, Deadline)) {
    return *EndStatus;
  }
  const grpc::Status Status = handleAnalyzeDivergences(
      HandlerContext, *Request, Response,
      makeClientCancelCheck(Context, Deadline), computeTimeBudget(Deadline));
  if (Status.ok()) {
    if (const auto EndStatus = contextEndStatus(Context, Deadline)) {
      return *EndStatus;
    }
  }
  return Status;
}

} // namespace worker
