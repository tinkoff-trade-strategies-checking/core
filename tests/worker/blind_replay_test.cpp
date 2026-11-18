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

  // Инструменты: lot_value_at_start задан для A (0) и B (1).
  ASSERT_EQ(Bv.instruments_size(), 2);
  EXPECT_EQ(Bv.instruments(0).instrument_index(), 0);
  EXPECT_TRUE(Bv.instruments(0).has_lot_value_at_start());
  EXPECT_EQ(Bv.instruments(1).instrument_index(), 1);
  EXPECT_TRUE(Bv.instruments(1).has_lot_value_at_start());

  // История: у первого бара A open = 100.00, volume_rel = 100.00 (без
  // прогрева).
  ASSERT_EQ(Bv.history_size(), BaseResp.history_size());
  EXPECT_EQ(Bv.history(0).instrument_index(), 0);
  EXPECT_EQ(d(Bv.history(0).open()), core::Decimal::parse("100.00"));
  EXPECT_EQ(d(Bv.history(0).volume_rel()), core::Decimal::parse("100.00"));

  // Сделки: 2 сделки (покупка на шаге 1 и продажа на шаге 3).
  ASSERT_EQ(Bv.fills_size(), 2);
  EXPECT_GT(d(Bv.fills(0).price()), core::Decimal::zero());
  EXPECT_GT(d(Bv.fills(0).value_pct()), core::Decimal::zero());
  EXPECT_GE(d(Bv.fills(0).commission_pct()), core::Decimal::zero());

  // Портфель: позиции и проценты капитала.
  ASSERT_EQ(Bv.portfolio().positions_size(), 1);
  EXPECT_EQ(Bv.portfolio().positions(0).instrument_index(), 0);
  EXPECT_EQ(Bv.portfolio().positions(0).lots(), 3);
  EXPECT_GT(d(Bv.portfolio().cash_pct()), core::Decimal::zero());
  EXPECT_GT(d(Bv.portfolio().equity_pct()), core::Decimal::zero());

  // Отчёт: кривая, издержки, пары FIFO.
  ASSERT_TRUE(Bv.has_report());
  EXPECT_EQ(Bv.report().equity_size(), BaseResp.report().equity_size());
  EXPECT_EQ(d(Bv.report().final_equity_pct()), d(Bv.portfolio().equity_pct()));
  ASSERT_EQ(Bv.report().round_trips_size(), 1);
  EXPECT_EQ(Bv.report().round_trips(0).instrument_index(), 0);
  EXPECT_EQ(Bv.report().round_trips(0).lots(), 2);
}

// INV-7: в BlindSessionView нет ни тикера/ключа бумаги, ни реального времени.
TEST(BlindReplaySession, BlindSessionViewHasNoTickersOrRealTimestamps) {
  EXPECT_EQ(pb::BlindBar::descriptor()->FindFieldByName("time_unix_ms"),
            nullptr);
  EXPECT_EQ(pb::BlindBar::descriptor()->FindFieldByName("instrument_key"),
            nullptr);
  EXPECT_EQ(
      pb::BlindInstrument::descriptor()->FindFieldByName("instrument_key"),
      nullptr);

  core::testing::Fixture Fixture = core::testing::fixture1();
  worker::Context Context{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};

  pb::ReplaySessionRequest Req;
  *Req.mutable_market() = fixtureMarket();
  Req.mutable_market()->set_warmup_bars(1);
  *Req.add_journal() = marketOrder(0, 0, "A", pb::BUY, 2);
  *Req.add_journal() = marketOrder(1, 2, "A", pb::SELL, 2);
  Req.set_up_to_step(4);
  Req.set_events_from_step(0);
  Req.set_include_history(true);
  Req.set_include_report(true);
  Req.set_blind(true);

  pb::ReplaySessionResponse Resp;
  ASSERT_TRUE(worker::handleReplaySession(Context, Req, &Resp).ok());
  ASSERT_TRUE(Resp.has_blind_view());
  const std::string Wire = Resp.blind_view().SerializeAsString();
  EXPECT_EQ(Wire.find("FIXA"), std::string::npos);
  EXPECT_EQ(Wire.find("FIXB"), std::string::npos);
}

// Бумага без бара на шаге 0: до появления P0 отсутствует в слепом виде
// целиком (включая бары прогрева); после появления первой свечи — видна с
// прогревом.
TEST(BlindReplaySession,
     InstrumentWithoutStepZeroBarIsAbsentFromBlindViewUntilFirstBar) {
  core::InMemoryBarSource Bars;
  core::InMemoryCalendarSource Calendar;
  core::InMemoryRuoniaSource Ruonia;

  const std::int64_t Day0 = 1736492400000LL; // 2025-01-10 10:00 MSK (прогрев)
  const std::int64_t Day1 = 1736751600000LL; // 2025-01-13 10:00 MSK (шаг 0)
  const std::int64_t StepMs = 60000LL;
  const std::int32_t IntervalSec = core::intervalSeconds(core::Timeframe::M1);

  auto MakeBar = [](std::int64_t Ts, const char *Open) {
    core::Bar B;
    B.TsOpenMs = Ts;
    B.AvailableAtMs = Ts + 60000LL;
    B.Open = core::Decimal::parse(Open);
    B.High = B.Open;
    B.Low = B.Open;
    B.Close = B.Open;
    B.Volume = 100;
    return B;
  };

  Bars.addSecurity("snap-gap", {"AAA", "TKA", 1, core::Decimal::parse("0.01")});
  Bars.addSecurity("snap-gap", {"BBB", "TKB", 1, core::Decimal::parse("0.01")});
  // Прогрев есть у обеих бумаг; на шаге 0 (Day1) бар есть только у AAA,
  // а у BBB первый бар появляется на шаге 1 (Day1 + StepMs).
  Bars.addSeries("snap-gap", "AAA", IntervalSec,
                 {MakeBar(Day0, "90"), MakeBar(Day1, "100"),
                  MakeBar(Day1 + StepMs, "102")});
  Bars.addSeries("snap-gap", "BBB", IntervalSec,
                 {MakeBar(Day0, "180"), MakeBar(Day1 + StepMs, "200")});
  Calendar.add("snap-gap", {{20250110, true}, {20250113, true}});

  pb::MarketSpec Market;
  Market.set_snapshot_id("snap-gap");
  pb::Instrument *InstA = Market.add_instruments();
  InstA->set_key("A");
  InstA->set_ticker("AAA");
  InstA->set_lot_size(1);
  *InstA->mutable_price_step() = dec("0.01");
  pb::Instrument *InstB = Market.add_instruments();
  InstB->set_key("B");
  InstB->set_ticker("BBB");
  InstB->set_lot_size(1);
  *InstB->mutable_price_step() = dec("0.01");
  Market.set_timeframe(pb::M1);
  Market.set_from_date("2025-01-13");
  Market.set_to_date("2025-01-13");
  Market.set_warmup_bars(1);
  pb::CostConfig *Costs = Market.mutable_costs();
  *Costs->mutable_commission_rate() = dec("0.0005");
  *Costs->mutable_exchange_fee_rate() = dec("0.0001");
  *Costs->mutable_spread() = dec("0.001");
  *Costs->mutable_slippage_kappa() = dec("0.1");
  *Costs->mutable_liquidity_share() = dec("0.10");
  *Market.mutable_initial_cash() = dec("100000");

  worker::Context Ctx{Bars, Calendar, Ruonia};

  // На шаге 0 у бумаги 1 (B) ещё нет P0: ни в history, ни в events её нет.
  pb::ReplaySessionRequest Req0;
  *Req0.mutable_market() = Market;
  Req0.set_up_to_step(0);
  Req0.set_events_from_step(0);
  Req0.set_include_history(true);
  Req0.set_blind(true);

  pb::ReplaySessionResponse Resp0;
  ASSERT_TRUE(worker::handleReplaySession(Ctx, Req0, &Resp0).ok());
  const pb::BlindSessionView &Bv0 = Resp0.blind_view();
  ASSERT_EQ(Bv0.instruments_size(), 2);
  EXPECT_TRUE(Bv0.instruments(0).has_lot_value_at_start());
  EXPECT_FALSE(Bv0.instruments(1).has_lot_value_at_start());
  for (const pb::BlindBar &B : Bv0.history())
    EXPECT_EQ(B.instrument_index(), 0);
  for (const pb::BlindStepEvents &Ev : Bv0.events()) {
    for (const pb::BlindBar &B : Ev.bars())
      EXPECT_EQ(B.instrument_index(), 0);
  }

  // На шаге 1 у бумаги 1 (B) появляется первый бар -> в history появляются и
  // её прогрев (step = -1), и бар шага 1.
  pb::ReplaySessionRequest Req1 = Req0;
  Req1.set_up_to_step(1);
  pb::ReplaySessionResponse Resp1;
  ASSERT_TRUE(worker::handleReplaySession(Ctx, Req1, &Resp1).ok());
  const pb::BlindSessionView &Bv1 = Resp1.blind_view();
  EXPECT_TRUE(Bv1.instruments(1).has_lot_value_at_start());
  int CountBWarmup = 0;
  int CountBSession = 0;
  for (const pb::BlindBar &B : Bv1.history()) {
    if (B.instrument_index() == 1) {
      if (B.step() < 0)
        ++CountBWarmup;
      else
        ++CountBSession;
    }
  }
  EXPECT_EQ(CountBWarmup, 1);
  EXPECT_EQ(CountBSession, 1);
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
