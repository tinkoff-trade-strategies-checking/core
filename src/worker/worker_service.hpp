#pragma once

#include <grpcpp/grpcpp.h>

#include "simulation.grpc.pb.h"

namespace worker {

// Реализация gRPC-сервиса SimulationWorker. Пока только Ping.
class WorkerService final : public pac::simulation::v1::SimulationWorker::Service {
 public:
  grpc::Status Ping(grpc::ServerContext* context,
                    const pac::simulation::v1::PingRequest* request,
                    pac::simulation::v1::PingResponse* response) override;
};

}  // namespace worker
