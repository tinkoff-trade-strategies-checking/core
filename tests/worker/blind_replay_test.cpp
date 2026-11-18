#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/market/bar.hpp"
#include "core/market/bar_source.hpp"
#include "core/money/decimal.hpp"
#include "core/money/proto_conv.hpp"
#include "worker_test_support.hpp"

namespace {

using namespace worker_test;

core::Decimal d(const pb::Decimal &Msg) { return core::fromProto(Msg); }

} // namespace

TEST(BlindReplaySession, BlindFalseOmitsBlindView) {
  core::testing::Fixture Fixture = core::testing::fixture1();
  worker::Context Context{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};
  pb::ReplaySessionRequest Request;
  *Request.mutable_market() = fixtureMarket();
  Request.set_up_to_step(3);
  Request.set_events_from_step(0);
  Request.set_include_history(true);
  Request.set_include_report(true);
  Request.set_blind(false);

  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(worker::handleReplaySession(Context, Request, &Response).ok());
  EXPECT_FALSE(Response.has_blind_view());
}

TEST(BlindReplaySession, BlindTrueKeepsRubleFieldsAndPopulatesBlindView) {
  core::testing::Fixture Fixture = core::testing::fixture1();
  worker::Context Context{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};

  pb::ReplaySessionRequest BaseReq;
  *BaseReq.mutable_market() = fixtureMarket();
  *BaseReq.add_journal() = marketOrder(0, 0, "A", pb::BUY, 5);
  *BaseReq.add_journal() = marketOrder(1, 2, "A", pb::SELL, 2);
  BaseReq.set_up_to_step(4);
  BaseReq.set_events_from_step(0);
  BaseReq.set_include_history(true);
  BaseReq.set_include_report(true);

  pb::ReplaySessionResponse BaseResp;
  ASSERT_TRUE(worker::handleReplaySession(Context, BaseReq, &BaseResp).ok());

  pb::ReplaySessionRequest BlindReq = BaseReq;
  BlindReq.set_blind(true);
  pb::ReplaySessionResponse BlindResp;
  ASSERT_TRUE(worker::handleReplaySession(Context, BlindReq, &BlindResp).ok());

  ASSERT_TRUE(BlindResp.has_blind_view());
  const pb::BlindSessionView &Bv = BlindResp.blind_view();

  // Обычные рублёвые поля остаются неизменными.
  EXPECT_EQ(BlindResp.portfolio().SerializeAsString(),
            BaseResp.portfolio().SerializeAsString());
  EXPECT_EQ(BlindResp.report().SerializeAsString(),
            BaseResp.report().SerializeAsString());
  EXPECT_EQ(BlindResp.fills_size(), BaseResp.fills_size());
  EXPECT_EQ(BlindResp.history_size(), BaseResp.history_size());

  // Инструменты: lot_value_at_start задан для A и B.
  ASSERT_EQ(Bv.instruments_size(), 2);
  EXPECT_EQ(Bv.instruments(0).instrument_key(), "A");
  EXPECT_TRUE(Bv.instruments(0).has_lot_value_at_start());
  EXPECT_EQ(Bv.instruments(1).instrument_key(), "B");
  EXPECT_TRUE(Bv.instruments(1).has_lot_value_at_start());

  // История: у первого бара A open = 100.00, volume_rel = 100.00 (без
  // прогрева).
  ASSERT_EQ(Bv.history_size(), BaseResp.history_size());
  EXPECT_EQ(Bv.history(0).instrument_key(), "A");
  EXPECT_EQ(d(Bv.history(0).open()), core::Decimal::parse("100.00"));
  EXPECT_EQ(d(Bv.history(0).volume_rel()), core::Decimal::parse("100.00"));

  // Сделки: 2 сделки (покупка на шаге 1 и продажа на шаге 3).
  ASSERT_EQ(Bv.fills_size(), 2);
  EXPECT_GT(d(Bv.fills(0).price()), core::Decimal::zero());
  EXPECT_GT(d(Bv.fills(0).value_pct()), core::Decimal::zero());
  EXPECT_GE(d(Bv.fills(0).commission_pct()), core::Decimal::zero());

  // Портфель: позиции и проценты капитала.
  ASSERT_EQ(Bv.portfolio().positions_size(), 1);
  EXPECT_EQ(Bv.portfolio().positions(0).instrument_key(), "A");
  EXPECT_EQ(Bv.portfolio().positions(0).lots(), 3);
  EXPECT_GT(d(Bv.portfolio().cash_pct()), core::Decimal::zero());
  EXPECT_GT(d(Bv.portfolio().equity_pct()), core::Decimal::zero());

  // Отчёт: кривая, издержки, пары FIFO.
  ASSERT_TRUE(Bv.has_report());
  EXPECT_EQ(Bv.report().equity_size(), BaseResp.report().equity_size());
  EXPECT_EQ(d(Bv.report().final_equity_pct()), d(Bv.portfolio().equity_pct()));
  ASSERT_EQ(Bv.report().round_trips_size(), 1);
  EXPECT_EQ(Bv.report().round_trips(0).instrument_key(), "A");
  EXPECT_EQ(Bv.report().round_trips(0).lots(), 2);
}

// INV-1: ответ (включая blind_view) до шага N не зависит от баров после N.
TEST(BlindReplaySession, NoFutureDataAffectsBlindView) {
  core::testing::Fixture F1 = core::testing::fixture1();
  core::testing::Fixture F2 = core::testing::fixture1();

  // Подменяем бары FIXA после шага 3 (индексы >= 4) в F2.
  const std::int32_t IntervalSec = core::intervalSeconds(core::Timeframe::D1);
  std::vector<core::Bar> BarsA = F2.Bars.load(
      core::BarRequest{"fixture-1", "FIXA", IntervalSec, 0, 2000000000000LL});
  for (std::size_t I = 4; I < BarsA.size(); ++I) {
    BarsA[I].Open = BarsA[I].Open.times(5);
    BarsA[I].High = BarsA[I].High.times(5);
    BarsA[I].Low = BarsA[I].Low.times(5);
    BarsA[I].Close = BarsA[I].Close.times(5);
    BarsA[I].Volume = BarsA[I].Volume * 100;
  }
  F2.Bars.addSeries("fixture-1", "FIXA", IntervalSec, BarsA);

  pb::ReplaySessionRequest Req;
  *Req.mutable_market() = fixtureMarket();
  *Req.add_journal() = marketOrder(0, 0, "A", pb::BUY, 5);
  Req.set_up_to_step(3);
  Req.set_events_from_step(0);
  Req.set_include_history(true);
  Req.set_include_report(true);
  Req.set_blind(true);

  worker::Context Ctx1{F1.Bars, F1.Calendar, F1.Ruonia};
  worker::Context Ctx2{F2.Bars, F2.Calendar, F2.Ruonia};

  pb::ReplaySessionResponse R1;
  pb::ReplaySessionResponse R2;
  ASSERT_TRUE(worker::handleReplaySession(Ctx1, Req, &R1).ok());
  ASSERT_TRUE(worker::handleReplaySession(Ctx2, Req, &R2).ok());

  EXPECT_EQ(R1.SerializeAsString(), R2.SerializeAsString());
}
