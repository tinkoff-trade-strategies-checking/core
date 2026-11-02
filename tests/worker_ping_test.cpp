#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "core/market/calendar_source.hpp"
#include "core/market/in_memory_bar_source.hpp"
#include "core/version.hpp"
#include "worker_service.hpp"

TEST(WorkerPing, ReturnsCoreVersion) {
  core::InMemoryBarSource bars;
  core::InMemoryCalendarSource calendar;
  worker::WorkerService service(worker::Context{bars, calendar});
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

TEST(ProtoContract, CompilesAndRoundTrips) {
  namespace pb = pac::simulation::v1;

  pb::ReplaySessionRequest request;
  request.mutable_market()->set_snapshot_id("11111111-2222-3333-4444-555555555555");
  request.mutable_market()->set_timeframe(pb::M5);
  request.mutable_market()->set_from_date("2024-01-09");
  request.mutable_market()->set_to_date("2024-03-29");
  pb::Instrument* instrument = request.mutable_market()->add_instruments();
  instrument->set_key("SBER");
  instrument->set_lot_size(10);
  instrument->mutable_price_step()->set_units(0);
  instrument->mutable_price_step()->set_nano(10000000);
  request.mutable_market()->mutable_initial_cash()->set_units(1000000);
  request.set_up_to_step(42);
  request.set_events_from_step(-1);

  pb::JournalEntry* entry = request.add_journal();
  entry->set_seq(7);
  entry->set_step(41);
  pb::OrderSpec* order = entry->mutable_place()->mutable_order();
  order->set_instrument_key("SBER");
  order->set_side(pb::BUY);
  order->set_type(pb::LIMIT);
  order->set_lots(3);
  order->mutable_limit_price()->set_units(271);
  order->mutable_limit_price()->set_nano(350000000);
  order->set_time_in_force(pb::GTC);

  std::string bytes;
  ASSERT_TRUE(request.SerializeToString(&bytes));
  pb::ReplaySessionRequest parsed;
  ASSERT_TRUE(parsed.ParseFromString(bytes));

  EXPECT_EQ(parsed.market().snapshot_id(), "11111111-2222-3333-4444-555555555555");
  EXPECT_EQ(parsed.market().timeframe(), pb::M5);
  EXPECT_EQ(parsed.market().instruments(0).key(), "SBER");
  EXPECT_EQ(parsed.market().instruments(0).price_step().nano(), 10000000);
  EXPECT_EQ(parsed.market().initial_cash().units(), 1000000);
  EXPECT_EQ(parsed.up_to_step(), 42);
  EXPECT_EQ(parsed.events_from_step(), -1);
  ASSERT_EQ(parsed.journal_size(), 1);
  const pb::JournalEntry& got = parsed.journal(0);
  EXPECT_EQ(got.seq(), 7);
  EXPECT_EQ(got.step(), 41);
  ASSERT_EQ(got.command_case(), pb::JournalEntry::kPlace);
  EXPECT_EQ(got.place().order().instrument_key(), "SBER");
  EXPECT_EQ(got.place().order().side(), pb::BUY);
  EXPECT_EQ(got.place().order().type(), pb::LIMIT);
  EXPECT_EQ(got.place().order().lots(), 3);
  EXPECT_EQ(got.place().order().time_in_force(), pb::GTC);
  EXPECT_EQ(got.place().order().limit_price().units(), 271);
  EXPECT_EQ(got.place().order().limit_price().nano(), 350000000);
  EXPECT_EQ(got.place().oco_with().lots(), 0);
}
