#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <memory>
#include <vector>

#include "convert/market.hpp"
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

TEST(WorkerEstimateOrder, ReturnsInsufficientCashOnDecimalOverflow) {
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
  ASSERT_TRUE(worker::handleEstimateOrder(Ctx, Req, &Resp).ok());
  EXPECT_FALSE(Resp.can_place());
  EXPECT_EQ(Resp.reason(), pb::INSUFFICIENT_CASH);
  EXPECT_EQ(Resp.invalid_field(), "");

  // Подача той же заявки в ReplaySession даёт ту же причину отказа.
  pb::ReplaySessionRequest ReplayReq;
  *ReplayReq.mutable_market() = fixtureMarket();
  ReplayReq.set_up_to_step(2);
  pb::JournalEntry *E = ReplayReq.add_journal();
  E->set_seq(0);
  E->set_step(2);
  *E->mutable_place()->mutable_order() = *Order;
  pb::ReplaySessionResponse ReplayResp;
  ASSERT_TRUE(worker::handleReplaySession(Ctx, ReplayReq, &ReplayResp).ok());
  ASSERT_EQ(ReplayResp.command_results_size(), 1);
  EXPECT_FALSE(ReplayResp.command_results(0).accepted());
  EXPECT_EQ(ReplayResp.command_results(0).reason(), Resp.reason());
  EXPECT_EQ(ReplayResp.command_results(0).invalid_field(),
            Resp.invalid_field());
}

TEST(WorkerEstimateOrder,
     BlindPricesConvertedToResolvedOrderAndErrorsReturnCanPlaceFalse) {
  core::testing::Fixture Fixture = core::testing::fixture1();
  const std::int32_t IntervalSec = core::intervalSeconds(core::Timeframe::D1);
  std::vector<core::Bar> BarsA = Fixture.Bars.load(
      core::BarRequest{"fixture-1", "FIXA", IntervalSec, 0, 2000000000000LL});
  // P0 != 100: open шага 0 = 200.00, price_step = 0.01.
  BarsA[0].Open = core::Decimal::parse("200.00");
  Fixture.Bars.addSeries("fixture-1", "FIXA", IntervalSec, BarsA);
  worker::Context Ctx{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};

  pb::EstimateOrderRequest Req;
  *Req.mutable_market() = fixtureMarket();
  Req.set_up_to_step(2);
  pb::OrderSpec *Order = Req.mutable_order();
  Order->set_instrument_key("A");
  Order->set_side(pb::BUY);
  Order->set_type(pb::LIMIT);
  Order->set_lots(1);
  Order->set_time_in_force(pb::DAY);
  Order->set_blind_prices(true);
  // 95.003 * 200 / 100 = 190.006 -> CEILING к 0.01 = 190.01.
  *Order->mutable_limit_price() = dec("95.003");

  pb::EstimateOrderResponse BuyResp;
  ASSERT_TRUE(worker::handleEstimateOrder(Ctx, Req, &BuyResp).ok());
  EXPECT_TRUE(BuyResp.can_place());
  EXPECT_EQ(d(BuyResp.reference_price()), BarsA[2].Close);
  ASSERT_TRUE(BuyResp.has_resolved_order());
  EXPECT_FALSE(BuyResp.resolved_order().blind_prices());
  EXPECT_EQ(d(BuyResp.resolved_order().limit_price()),
            core::Decimal::parse("190.01"));

  // Продажа: FLOOR к 0.01: 105.003 * 200 / 100 = 210.006 -> 210.00.
  Order->set_side(pb::SELL);
  *Order->mutable_limit_price() = dec("105.003");
  pb::EstimateOrderResponse SellResp;
  ASSERT_TRUE(worker::handleEstimateOrder(Ctx, Req, &SellResp).ok());
  EXPECT_FALSE(SellResp.can_place());
  EXPECT_EQ(SellResp.reason(), pb::INSUFFICIENT_POSITION);
  ASSERT_TRUE(SellResp.has_resolved_order());
  EXPECT_FALSE(SellResp.resolved_order().blind_prices());
  EXPECT_EQ(d(SellResp.resolved_order().limit_price()),
            core::Decimal::parse("210.00"));

  // Нет P0 у бумаги B на шаге 0 -> can_place = false,
  // INVALID_PARAMS/instrument.
  std::vector<core::Bar> BarsB = Fixture.Bars.load(
      core::BarRequest{"fixture-1", "FIXB", IntervalSec, 0, 2000000000000LL});
  BarsB.erase(BarsB.begin());
  Fixture.Bars.addSeries("fixture-1", "FIXB", IntervalSec, BarsB);
  Req.set_up_to_step(0);
  Order->set_instrument_key("B");
  Order->set_side(pb::BUY);
  *Order->mutable_limit_price() = dec("100.00");
  pb::EstimateOrderResponse NoP0Resp;
  ASSERT_TRUE(worker::handleEstimateOrder(Ctx, Req, &NoP0Resp).ok());
  EXPECT_FALSE(NoP0Resp.can_place());
  EXPECT_EQ(NoP0Resp.reason(), pb::INVALID_PARAMS);
  EXPECT_EQ(NoP0Resp.invalid_field(), "instrument");
  EXPECT_FALSE(NoP0Resp.has_resolved_order());

  // Переполнение при переводе слепой цены -> can_place = false,
  // INVALID_PARAMS/limitPrice (RPC не падает).
  BarsA[0].Open = core::Decimal::parse("1000000000000000000");
  Fixture.Bars.addSeries("fixture-1", "FIXA", IntervalSec, BarsA);
  Req.set_up_to_step(2);
  Order->set_instrument_key("A");
  *Order->mutable_limit_price() = dec("1000000000000000000");
  pb::EstimateOrderResponse OverflowResp;
  ASSERT_TRUE(worker::handleEstimateOrder(Ctx, Req, &OverflowResp).ok());
  EXPECT_FALSE(OverflowResp.can_place());
  EXPECT_EQ(OverflowResp.reason(), pb::INVALID_PARAMS);
  EXPECT_EQ(OverflowResp.invalid_field(), "limitPrice");
  EXPECT_FALSE(OverflowResp.has_resolved_order());
}

TEST(StatusFromCurrentException, DecimalOverflowIsInternal) {
  try {
    throw core::DecimalOverflow("internal overflow");
  } catch (...) {
    const grpc::Status S = worker::statusFromCurrentException();
    EXPECT_EQ(S.error_code(), grpc::StatusCode::INTERNAL);
  }
}
