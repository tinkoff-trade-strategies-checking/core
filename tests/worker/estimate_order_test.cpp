#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <memory>
#include <vector>

#include "core/market/bar.hpp"
#include "core/money/decimal.hpp"
#include "core/money/proto_conv.hpp"
#include "worker_service.hpp"
#include "worker_test_support.hpp"

namespace {

using namespace worker_test;

core::Decimal d(const pb::Decimal &Msg) { return core::fromProto(Msg); }

} // namespace

TEST(WorkerEstimateOrder, MarketAndLimitViaRpc) {
  core::testing::Fixture Fixture = core::testing::fixture1();
  worker::Context Ctx{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};
  worker::WorkerService Service(Ctx);

  grpc::ServerBuilder Builder;
  Builder.RegisterService(&Service);
  std::unique_ptr<grpc::Server> Server = Builder.BuildAndStart();
  std::unique_ptr<pb::SimulationWorker::Stub> Stub =
      pb::SimulationWorker::NewStub(
          Server->InProcessChannel(grpc::ChannelArguments()));

  pb::EstimateOrderRequest Req;
  *Req.mutable_market() = fixtureMarket();
  Req.set_up_to_step(2);
  pb::OrderSpec *Order = Req.mutable_order();
  Order->set_instrument_key("A");
  Order->set_side(pb::BUY);
  Order->set_type(pb::MARKET);
  Order->set_lots(5);
  Order->set_time_in_force(pb::DAY);

  pb::EstimateOrderResponse MarketResp;
  grpc::ClientContext ClientCtx1;
  ASSERT_TRUE(Stub->EstimateOrder(&ClientCtx1, Req, &MarketResp).ok());
  EXPECT_TRUE(MarketResp.can_place());
  EXPECT_EQ(MarketResp.reason(), pb::REASON_UNSPECIFIED);
  EXPECT_GT(d(MarketResp.reference_price()), core::Decimal::zero());
  EXPECT_GT(d(MarketResp.amount()), core::Decimal::zero());
  EXPECT_GT(d(MarketResp.commission()), core::Decimal::zero());
  EXPECT_GT(d(MarketResp.exchange_fee()), core::Decimal::zero());
  EXPECT_GT(d(MarketResp.required_cash()), d(MarketResp.amount()));
  EXPECT_GT(d(MarketResp.amount_pct()), core::Decimal::zero());
  EXPECT_GE(d(MarketResp.required_cash_pct()), d(MarketResp.amount_pct()));

  // Лимитная заявка на покупку: оценка по close последнего бара.
  Order->set_type(pb::LIMIT);
  *Order->mutable_limit_price() = dec("90.00");
  pb::EstimateOrderResponse LimitResp;
  grpc::ClientContext ClientCtx2;
  ASSERT_TRUE(Stub->EstimateOrder(&ClientCtx2, Req, &LimitResp).ok());
  EXPECT_EQ(MarketResp.SerializeAsString(), LimitResp.SerializeAsString());

  Server->Shutdown();
}

TEST(WorkerEstimateOrder, RejectsOutOfRangeStepAndMissingOrder) {
  core::testing::Fixture Fixture = core::testing::fixture1();
  worker::Context Ctx{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};

  pb::EstimateOrderRequest Req;
  *Req.mutable_market() = fixtureMarket();
  Req.set_up_to_step(100);
  pb::EstimateOrderResponse Resp;
  EXPECT_EQ(worker::handleEstimateOrder(Ctx, Req, &Resp).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  Req.set_up_to_step(2);
  EXPECT_EQ(worker::handleEstimateOrder(Ctx, Req, &Resp).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

// INV-1: ответ EstimateOrder на шаге N не зависит от баров после N.
TEST(WorkerEstimateOrder, NoFutureBarsAffectRpcEstimate) {
  core::testing::Fixture F1 = core::testing::fixture1();
  core::testing::Fixture F2 = core::testing::fixture1();

  const std::int32_t IntervalSec = core::intervalSeconds(core::Timeframe::D1);
  std::vector<core::Bar> BarsA = F2.Bars.load(
      core::BarRequest{"fixture-1", "FIXA", IntervalSec, 0, 2000000000000LL});
  for (std::size_t I = 4; I < BarsA.size(); ++I) {
    BarsA[I].Open = BarsA[I].Open.times(10);
    BarsA[I].High = BarsA[I].High.times(10);
    BarsA[I].Low = BarsA[I].Low.times(10);
    BarsA[I].Close = BarsA[I].Close.times(10);
  }
  F2.Bars.addSeries("fixture-1", "FIXA", IntervalSec, BarsA);

  pb::EstimateOrderRequest Req;
  *Req.mutable_market() = fixtureMarket();
  *Req.add_journal() = marketOrder(0, 0, "A", pb::BUY, 2);
  Req.set_up_to_step(3);
  pb::OrderSpec *Order = Req.mutable_order();
  Order->set_instrument_key("A");
  Order->set_side(pb::BUY);
  Order->set_type(pb::MARKET);
  Order->set_lots(3);
  Order->set_time_in_force(pb::GTC);

  worker::Context Ctx1{F1.Bars, F1.Calendar, F1.Ruonia};
  worker::Context Ctx2{F2.Bars, F2.Calendar, F2.Ruonia};
  pb::EstimateOrderResponse R1;
  pb::EstimateOrderResponse R2;
  ASSERT_TRUE(worker::handleEstimateOrder(Ctx1, Req, &R1).ok());
  ASSERT_TRUE(worker::handleEstimateOrder(Ctx2, Req, &R2).ok());
  EXPECT_EQ(R1.SerializeAsString(), R2.SerializeAsString());
}

TEST(WorkerEstimateOrder, ReturnsInvalidArgumentOnDecimalOverflow) {
  core::testing::Fixture Fixture = core::testing::fixture1();
  const std::int32_t IntervalSec = core::intervalSeconds(core::Timeframe::D1);
  std::vector<core::Bar> BarsA = Fixture.Bars.load(
      core::BarRequest{"fixture-1", "FIXA", IntervalSec, 0, 2000000000000LL});
  BarsA[2].Close = core::Decimal::parse("1000000000000000");
  Fixture.Bars.addSeries("fixture-1", "FIXA", IntervalSec, BarsA);
  worker::Context Ctx{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};

  pb::EstimateOrderRequest Req;
  *Req.mutable_market() = fixtureMarket();
  Req.set_up_to_step(2);
  pb::OrderSpec *Order = Req.mutable_order();
  Order->set_instrument_key("A");
  Order->set_side(pb::BUY);
  Order->set_type(pb::MARKET);
  Order->set_lots(9223372036854775807LL);
  Order->set_time_in_force(pb::DAY);

  pb::EstimateOrderResponse Resp;
  const grpc::Status Status = worker::handleEstimateOrder(Ctx, Req, &Resp);
  EXPECT_EQ(Status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(Status.error_message().find("lots"), std::string::npos);
}

TEST(WorkerEstimateOrder, BlindPriceViaRpc) {
  core::testing::Fixture Fixture = core::testing::fixture1();
  worker::Context Ctx{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};

  pb::EstimateOrderRequest Req;
  *Req.mutable_market() = fixtureMarket();
  Req.set_up_to_step(2);
  pb::OrderSpec *Order = Req.mutable_order();
  Order->set_instrument_key("A");
  Order->set_side(pb::BUY);
  Order->set_type(pb::LIMIT);
  Order->set_lots(5);
  // В fixture-1 у FIXA Open шага 0 = 100.00.
  // Слепая цена 105.00 соответствует рублёвой 105.00
  *Order->mutable_limit_price() = dec("105.00");
  Order->set_is_blind_price(true);
  Order->set_time_in_force(pb::DAY);

  pb::EstimateOrderResponse Resp;
  const grpc::Status Status = worker::handleEstimateOrder(Ctx, Req, &Resp);
  ASSERT_TRUE(Status.ok());
  EXPECT_TRUE(Resp.can_place());
  EXPECT_GT(d(Resp.amount()), core::Decimal::zero());
  EXPECT_GT(d(Resp.amount_pct()), core::Decimal::zero());
}
