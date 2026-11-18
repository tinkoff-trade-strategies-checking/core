#pragma once

#include <grpcpp/grpcpp.h>

#include "handlers.hpp"
#include "simulation.grpc.pb.h"

namespace worker {

// Реализация gRPC-сервиса SimulationWorker. Ping отвечает сам, остальные RPC
// передают запрос обработчикам из handlers.hpp (по одному файлу
// handlers/<имя>.cpp на RPC).
class WorkerService final : public pb::SimulationWorker::Service {
public:
  // Источники данных должны пережить сервис.
  explicit WorkerService(Context Ctx) : HandlerContext(Ctx) {}

  grpc::Status Ping(grpc::ServerContext *Context,
                    const pb::PingRequest *Request,
                    pb::PingResponse *Response) override;
  grpc::Status ReplaySession(grpc::ServerContext *Context,
                             const pb::ReplaySessionRequest *Request,
                             pb::ReplaySessionResponse *Response) override;
  grpc::Status GetBars(grpc::ServerContext *Context,
                       const pb::GetBarsRequest *Request,
                       pb::GetBarsResponse *Response) override;
  grpc::Status EstimateOrder(grpc::ServerContext *Context,
                             const pb::EstimateOrderRequest *Request,
                             pb::EstimateOrderResponse *Response) override;
  grpc::Status ClassifyPeriods(grpc::ServerContext *Context,
                               const pb::ClassifyPeriodsRequest *Request,
                               pb::ClassifyPeriodsResponse *Response) override;

private:
  Context HandlerContext;
};

} // namespace worker
