#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "core/version.hpp"
#include "worker_service.hpp"

TEST(WorkerPing, ReturnsCoreVersion) {
  worker::WorkerService service;
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  ASSERT_TRUE(server);
  ASSERT_NE(port, 0);

  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                     grpc::InsecureChannelCredentials());
  auto stub = pac::simulation::v1::SimulationWorker::NewStub(channel);

  grpc::ClientContext context;
  pac::simulation::v1::PingRequest request;
  pac::simulation::v1::PingResponse response;
  const grpc::Status status = stub->Ping(&context, request, &response);

  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.core_version(), core::version());
  server->Shutdown();
}
