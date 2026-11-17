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
    Service = std::make_unique<worker::WorkerService>(
        worker::Context{Bars, Calendar});
    int Port = 0;
    grpc::ServerBuilder Builder;
    Builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &Port);
    Builder.RegisterService(Service.get());
    Server = Builder.BuildAndStart();
    ASSERT_TRUE(Server);
    ASSERT_NE(Port, 0);
    Stub = pb::SimulationWorker::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(Port),
                            grpc::InsecureChannelCredentials()));
  }

  void TearDown() override {
    if (Server)
      Server->Shutdown();
  }

  core::InMemoryBarSource Bars;
  core::InMemoryCalendarSource Calendar;
  std::unique_ptr<worker::WorkerService> Service;
  std::unique_ptr<grpc::Server> Server;
  std::unique_ptr<pb::SimulationWorker::Stub> Stub;
};

} // namespace

TEST_F(HandlersScaffold, PingReturnsCoreVersion) {
  grpc::ClientContext Context;
  pb::PingResponse Response;
  const grpc::Status Status = Stub->Ping(&Context, {}, &Response);
  EXPECT_TRUE(Status.ok()) << Status.error_message();
  EXPECT_EQ(Response.core_version(), core::version());
}
