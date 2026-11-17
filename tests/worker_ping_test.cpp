#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "core/market/calendar_source.hpp"
#include "core/market/in_memory_bar_source.hpp"
#include "core/version.hpp"
#include "worker_service.hpp"

TEST(WorkerPing, ReturnsCoreVersion) {
  core::InMemoryBarSource Bars;
  core::InMemoryCalendarSource Calendar;
  worker::WorkerService Service(worker::Context{Bars, Calendar});
  int Port = 0;
  grpc::ServerBuilder Builder;
  Builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &Port);
  Builder.RegisterService(&Service);
  std::unique_ptr<grpc::Server> Server = Builder.BuildAndStart();
  ASSERT_TRUE(Server);
  ASSERT_NE(Port, 0);

  auto Channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(Port),
                                     grpc::InsecureChannelCredentials());
  auto Stub = pac::simulation::v1::SimulationWorker::NewStub(Channel);

  grpc::ClientContext Context;
  pac::simulation::v1::PingRequest Request;
  pac::simulation::v1::PingResponse Response;
  const grpc::Status Status = Stub->Ping(&Context, Request, &Response);

  EXPECT_TRUE(Status.ok()) << Status.error_message();
  EXPECT_EQ(Response.core_version(), core::version());
  Server->Shutdown();
}

TEST(ProtoContract, CompilesAndRoundTrips) {
  namespace pb = pac::simulation::v1;

  pb::ReplaySessionRequest Request;
  Request.mutable_market()->set_snapshot_id(
      "11111111-2222-3333-4444-555555555555");
  Request.mutable_market()->set_timeframe(pb::M5);
  Request.mutable_market()->set_from_date("2024-01-09");
  Request.mutable_market()->set_to_date("2024-03-29");
  pb::Instrument *Instrument = Request.mutable_market()->add_instruments();
  Instrument->set_key("SBER");
  Instrument->set_lot_size(10);
  Instrument->mutable_price_step()->set_units(0);
  Instrument->mutable_price_step()->set_nano(10000000);
  Request.mutable_market()->mutable_initial_cash()->set_units(1000000);
  Request.set_up_to_step(42);
  Request.set_events_from_step(-1);

  pb::JournalEntry *Entry = Request.add_journal();
  Entry->set_seq(7);
  Entry->set_step(41);
  pb::OrderSpec *Order = Entry->mutable_place()->mutable_order();
  Order->set_instrument_key("SBER");
  Order->set_side(pb::BUY);
  Order->set_type(pb::LIMIT);
  Order->set_lots(3);
  Order->mutable_limit_price()->set_units(271);
  Order->mutable_limit_price()->set_nano(350000000);
  Order->set_time_in_force(pb::GTC);

  std::string Bytes;
  ASSERT_TRUE(Request.SerializeToString(&Bytes));
  pb::ReplaySessionRequest Parsed;
  ASSERT_TRUE(Parsed.ParseFromString(Bytes));

  EXPECT_EQ(Parsed.market().snapshot_id(),
            "11111111-2222-3333-4444-555555555555");
  EXPECT_EQ(Parsed.market().timeframe(), pb::M5);
  EXPECT_EQ(Parsed.market().instruments(0).key(), "SBER");
  EXPECT_EQ(Parsed.market().instruments(0).price_step().nano(), 10000000);
  EXPECT_EQ(Parsed.market().initial_cash().units(), 1000000);
  EXPECT_EQ(Parsed.up_to_step(), 42);
  EXPECT_EQ(Parsed.events_from_step(), -1);
  ASSERT_EQ(Parsed.journal_size(), 1);
  const pb::JournalEntry &Got = Parsed.journal(0);
  EXPECT_EQ(Got.seq(), 7);
  EXPECT_EQ(Got.step(), 41);
  ASSERT_EQ(Got.command_case(), pb::JournalEntry::kPlace);
  EXPECT_EQ(Got.place().order().instrument_key(), "SBER");
  EXPECT_EQ(Got.place().order().side(), pb::BUY);
  EXPECT_EQ(Got.place().order().type(), pb::LIMIT);
  EXPECT_EQ(Got.place().order().lots(), 3);
  EXPECT_EQ(Got.place().order().time_in_force(), pb::GTC);
  EXPECT_EQ(Got.place().order().limit_price().units(), 271);
  EXPECT_EQ(Got.place().order().limit_price().nano(), 350000000);
  EXPECT_EQ(Got.place().oco_with().lots(), 0);
}
