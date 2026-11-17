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
  explicit WorkerService(Context context) : context_(context) {}

  grpc::Status Ping(grpc::ServerContext *context,
                    const pb::PingRequest *request,
                    pb::PingResponse *response) override;
  grpc::Status CheckStrategy(grpc::ServerContext *context,
                             const pb::CheckStrategyRequest *request,
                             pb::CheckStrategyResponse *response) override;
  grpc::Status ReplaySession(grpc::ServerContext *context,
                             const pb::ReplaySessionRequest *request,
                             pb::ReplaySessionResponse *response) override;
  grpc::Status GetBars(grpc::ServerContext *context,
                       const pb::GetBarsRequest *request,
                       pb::GetBarsResponse *response) override;
  grpc::Status
  RunBacktest(grpc::ServerContext *context,
              const pb::RunBacktestRequest *request,
              grpc::ServerWriter<pb::RunBacktestUpdate> *writer) override;
  grpc::Status
  RunWalkForward(grpc::ServerContext *context,
                 const pb::WalkForwardRequest *request,
                 grpc::ServerWriter<pb::WalkForwardUpdate> *writer) override;
  grpc::Status AnalyzeDivergences(grpc::ServerContext *context,
                                  const pb::DivergenceRequest *request,
                                  pb::DivergenceResponse *response) override;

private:
  Context context_;
};

} // namespace worker
