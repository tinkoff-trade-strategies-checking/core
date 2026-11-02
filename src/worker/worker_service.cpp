#include "worker_service.hpp"

#include <string>

#include "core/version.hpp"

namespace worker {

grpc::Status WorkerService::Ping(grpc::ServerContext* /*context*/,
                                 const pac::simulation::v1::PingRequest* /*request*/,
                                 pac::simulation::v1::PingResponse* response) {
  response->set_core_version(std::string(core::version()));
  return grpc::Status::OK;
}

}  // namespace worker
