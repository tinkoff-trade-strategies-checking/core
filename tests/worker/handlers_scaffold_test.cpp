#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "core/market/calendar_source.hpp"
#include "core/market/in_memory_bar_source.hpp"
#include "core/version.hpp"
#include "worker_service.hpp"

namespace {

namespace pb = pac::simulation::v1;

// Сервис в процессе на свободном порту и клиент к нему.
class HandlersScaffold : public ::testing::Test {
 protected:
  void SetUp() override {
    service_ = std::make_unique<worker::WorkerService>(worker::Context{bars_, calendar_});
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    ASSERT_TRUE(server_);
    ASSERT_NE(port, 0);
    stub_ = pb::SimulationWorker::NewStub(grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  }

  void TearDown() override {
    if (server_) server_->Shutdown();
  }

  core::InMemoryBarSource bars_;
  core::InMemoryCalendarSource calendar_;
  std::unique_ptr<worker::WorkerService> service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<pb::SimulationWorker::Stub> stub_;
};

}  // namespace

TEST_F(HandlersScaffold, PingReturnsCoreVersion) {
  grpc::ClientContext context;
  pb::PingResponse response;
  const grpc::Status status = stub_->Ping(&context, {}, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.core_version(), core::version());
}
