#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "convert/market.hpp"
#include "core/blind/blind.hpp"
#include "core/market/bar.hpp"
#include "core/market/bar_source.hpp"
#include "core/market/ruonia_source.hpp"
#include "core/money/proto_conv.hpp"
#include "core/report/derived.hpp"
#include "worker_test_support.hpp"

namespace {

using namespace worker_test;

class ReplaySession : public ::testing::Test {
protected:
  grpc::Status replay(std::int64_t UpTo, std::int64_t EventsFrom,
                      pb::ReplaySessionResponse *Response, bool History = true,
                      bool Report = false) {
    worker::Context Context{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};
    pb::ReplaySessionRequest Request;
    *Request.mutable_market() = Market;
    for (const pb::JournalEntry &E : Journal)
      *Request.add_journal() = E;
    Request.set_up_to_step(UpTo);
    Request.set_events_from_step(EventsFrom);
    Request.set_include_history(History);
    Request.set_include_report(Report);
    return worker::handleReplaySession(Context, Request, Response);
  }

  core::testing::Fixture Fixture = core::testing::fixture1();
  pb::MarketSpec Market = fixtureMarket();
  std::vector<pb::JournalEntry> Journal;
};

} // namespace

// INV-1: ни history, ни events не содержат шагов позже up_to_step.
TEST_F(ReplaySession, UpToStepNoFuture) {
  Journal = {marketOrder(0, 0, "A", pb::BUY, 5)};
  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(replay(3, 0, &Response).ok());

  EXPECT_EQ(Response.total_steps(), 10);
  EXPECT_EQ(Response.status(), pb::RUNNING);
  EXPECT_EQ(Response.last_step(), 3);

  // История: шаги 0…3, у A бар на каждом, у B нет бара на шаге 3.
  ASSERT_EQ(Response.history_size(), 7);
  for (const pb::Bar &B : Response.history())
    EXPECT_LE(B.step(), 3);
  EXPECT_EQ(Response.history(6).step(), 3);

  // События (0; 3]: шаги 1, 2, 3.
  ASSERT_EQ(Response.events_size(), 3);
  for (int I = 0; I < Response.events_size(); ++I) {
    const pb::StepEvents &E = Response.events(I);
    EXPECT_EQ(E.step(), I + 1);
    for (const pb::Bar &B : E.bars())
      EXPECT_EQ(B.step(), E.step());
    for (const pb::Fill &F : E.fills())
      EXPECT_LE(F.step(), 3);
  }
  for (const pb::Fill &F : Response.fills())
    EXPECT_LE(F.step(), 3);
  // Покупка исполнилась на открытии шага 1 (INV-2), и событие шага 1 её
  // содержит.
  ASSERT_EQ(Response.fills_size(), 1);
  EXPECT_EQ(Response.fills(0).step(), 1);
  EXPECT_EQ(Response.fills(0).instrument_key(), "A");
  EXPECT_EQ(Response.fills(0).side(), pb::BUY);
  EXPECT_EQ(Response.fills(0).lots(), 5);
  ASSERT_EQ(Response.events(0).fills_size(), 1);

  ASSERT_EQ(Response.portfolio().positions_size(), 1);
  EXPECT_EQ(Response.portfolio().positions(0).instrument_key(), "A");
  EXPECT_EQ(Response.portfolio().positions(0).lots(), 5);
  // Стоимость позиции по close шага 3: 106 * 5 лотов * 10 штук.
  EXPECT_EQ(Response.portfolio().positions(0).market_value().units(), 5300);
  EXPECT_FALSE(Response.has_report());
}

TEST_F(ReplaySession, EventsFromStepIsExclusive) {
  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(replay(5, 3, &Response, false).ok());
  ASSERT_EQ(Response.events_size(), 2); // (3; 5]
  EXPECT_EQ(Response.events(0).step(), 4);
  EXPECT_EQ(Response.events(1).step(), 5);
  EXPECT_EQ(Response.history_size(), 0);

  pb::ReplaySessionResponse None;
  ASSERT_TRUE(replay(5, -1, &None, false).ok());
  EXPECT_EQ(None.events_size(), 0);

  pb::ReplaySessionResponse AtCursor;
  ASSERT_TRUE(replay(5, 5, &AtCursor, false).ok());
  EXPECT_EQ(AtCursor.events_size(), 0);
}

TEST_F(ReplaySession, CommandResultsPerEntry) {
  Journal = {marketOrder(0, 0, "A", pb::BUY, 5),
             marketOrder(1, 1, "A", pb::SELL, 100), // позиции всего 5 лотов
             marketOrder(2, 2, "A", pb::BUY, 0)}; // неверное количество
  pb::JournalEntry Cancel;
  Cancel.set_seq(3);
  Cancel.set_step(3);
  Cancel.mutable_cancel()->set_order_id(0); // заявка 0 уже исполнена
  Journal.push_back(Cancel);

  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(replay(4, -1, &Response).ok());
  ASSERT_EQ(Response.command_results_size(), 4);

  const pb::CommandResult &First = Response.command_results(0);
  EXPECT_EQ(First.seq(), 0);
  EXPECT_TRUE(First.accepted());
  ASSERT_EQ(First.order_ids_size(), 1);
  EXPECT_EQ(First.order_ids(0), 0);

  const pb::CommandResult &Oversell = Response.command_results(1);
  EXPECT_EQ(Oversell.seq(), 1);
  EXPECT_FALSE(Oversell.accepted());
  EXPECT_EQ(Oversell.reason(), pb::INSUFFICIENT_POSITION);
  ASSERT_EQ(Oversell.order_ids_size(), 1);
  EXPECT_EQ(Oversell.order_ids(0), 2); // seq 1, нога 0

  const pb::CommandResult &Zero = Response.command_results(2);
  EXPECT_FALSE(Zero.accepted());
  EXPECT_EQ(Zero.reason(), pb::INVALID_PARAMS);
  EXPECT_EQ(Zero.invalid_field(), "lots");

  const pb::CommandResult &Late = Response.command_results(3);
  EXPECT_FALSE(Late.accepted());
  EXPECT_EQ(Late.reason(), pb::ORDER_NOT_ACTIVE);
  EXPECT_EQ(Late.order_ids_size(), 0);

  // Отклонённые заявки остаются в журнале заявок со статусом REJECTED.
  ASSERT_EQ(Response.orders_size(), 3);
  EXPECT_EQ(Response.orders(0).id(), 0);
  EXPECT_EQ(Response.orders(0).status(), pb::FILLED);
  EXPECT_EQ(Response.orders(0).spec().instrument_key(), "A");
  EXPECT_EQ(Response.orders(0).spec().type(), pb::MARKET);
  EXPECT_FALSE(Response.orders(0).spec().has_limit_price());
  EXPECT_EQ(Response.orders(1).id(), 2);
  EXPECT_EQ(Response.orders(1).status(), pb::REJECTED);
  EXPECT_EQ(Response.orders(1).reason(), pb::INSUFFICIENT_POSITION);
  EXPECT_EQ(Response.orders(1).oco_sibling_id(), -1);
}

TEST_F(ReplaySession, LimitOrderKeepsPrices) {
  pb::JournalEntry E = marketOrder(0, 0, "A", pb::BUY, 2);
  pb::OrderSpec *Spec = E.mutable_place()->mutable_order();
  Spec->set_type(pb::LIMIT);
  *Spec->mutable_limit_price() = dec("90.5");
  Journal = {E};
  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(replay(2, -1, &Response, false).ok());
  ASSERT_EQ(Response.orders_size(), 1);
  const pb::Order &Order = Response.orders(0);
  EXPECT_EQ(Order.status(), pb::ACTIVE);
  EXPECT_EQ(Order.spec().type(), pb::LIMIT);
  ASSERT_TRUE(Order.spec().has_limit_price());
  EXPECT_EQ(Order.spec().limit_price().units(), 90);
  EXPECT_EQ(Order.spec().limit_price().nano(), 500000000);
  EXPECT_FALSE(Order.spec().has_stop_price());
}

TEST_F(ReplaySession, OcoPairGetsTwoIds) {
  // Покупка 5 лотов, затем OCO-продажа: стоп ниже рынка и тейк-профит выше.
  Journal = {marketOrder(0, 0, "A", pb::BUY, 5)};
  pb::JournalEntry Oco;
  Oco.set_seq(1);
  Oco.set_step(1);
  pb::PlaceOrder *Place = Oco.mutable_place();
  pb::OrderSpec *Stop = Place->mutable_order();
  Stop->set_instrument_key("A");
  Stop->set_side(pb::SELL);
  Stop->set_type(pb::STOP);
  Stop->set_lots(5);
  *Stop->mutable_stop_price() = dec("90");
  Stop->set_time_in_force(pb::GTC);
  pb::OrderSpec *Take = Place->mutable_oco_with();
  Take->set_instrument_key("A");
  Take->set_side(pb::SELL);
  Take->set_type(pb::TAKE_PROFIT);
  Take->set_lots(5);
  *Take->mutable_limit_price() = dec("200");
  Take->set_time_in_force(pb::GTC);
  Journal.push_back(Oco);

  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(replay(2, -1, &Response, false).ok());
  ASSERT_EQ(Response.command_results_size(), 2);
  const pb::CommandResult &R = Response.command_results(1);
  EXPECT_TRUE(R.accepted());
  ASSERT_EQ(R.order_ids_size(), 2);
  EXPECT_EQ(R.order_ids(0), 2);
  EXPECT_EQ(R.order_ids(1), 3);
  ASSERT_EQ(Response.orders_size(), 3);
  EXPECT_EQ(Response.orders(1).oco_sibling_id(), 3);
  EXPECT_EQ(Response.orders(2).oco_sibling_id(), 2);
}

TEST_F(ReplaySession, ReportOnRequest) {
  Journal = {marketOrder(0, 0, "A", pb::BUY, 5)};
  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(replay(9, -1, &Response, false, true).ok());
  EXPECT_EQ(Response.status(), pb::FINISHED_END);
  EXPECT_EQ(Response.last_step(), 9);
  ASSERT_TRUE(Response.has_report());
  const pb::Report &Report = Response.report();

  // final_equity - стоимость портфеля на last_step.
  EXPECT_EQ(Report.final_equity().units(),
            Response.portfolio().equity().units());
  EXPECT_EQ(Report.final_equity().nano(), Response.portfolio().equity().nano());
  ASSERT_EQ(Report.equity_size(), 10);
  EXPECT_EQ(Report.equity(9).step(), 9);
  EXPECT_EQ(Report.equity(9).equity().units(), Report.final_equity().units());

  const double FinalEquity =
      static_cast<double>(Report.final_equity().units()) +
      static_cast<double>(Report.final_equity().nano()) * 1e-9;
  EXPECT_NEAR(Report.metrics().total_return(), FinalEquity / 1000000.0 - 1.0,
              1e-9);
  EXPECT_EQ(Report.metrics().fills(), 1);
  EXPECT_EQ(Report.metrics().trading_days(), 10);
  EXPECT_FALSE(Report.metrics().has_benchmark_return());
  // Комиссия и сбор покупки в итогах издержек.
  EXPECT_GT(Report.costs().commission().units() +
                Report.costs().commission().nano(),
            0);

  pb::ReplaySessionResponse Without;
  ASSERT_TRUE(replay(9, -1, &Without, false, false).ok());
  EXPECT_FALSE(Without.has_report());
}

// Бенчмарк в отчёте не заглядывает в будущее (INV-1): считается по последний
// полностью пройденный день.
TEST_F(ReplaySession, BenchmarkStopsAtCursor) {
  std::vector<core::Bar> Imoex = Fixture.Bars.load(core::BarRequest{
      "fixture-1", "FIXA", 86400, 0, std::numeric_limits<std::int64_t>::max()});
  ASSERT_EQ(Imoex.size(), 10U);
  for (std::size_t I = 0; I < Imoex.size(); ++I) {
    Imoex[I].Open =
        core::Decimal::fromUnits(1000 + 10 * static_cast<std::int64_t>(I));
    Imoex[I].Close =
        core::Decimal::fromUnits(1005 + 10 * static_cast<std::int64_t>(I));
  }
  Fixture.Bars.addSecurity(
      "fixture-1",
      core::Security{"IMOEX", "", 1, core::Decimal::parse("0.01")});
  Fixture.Bars.addSeries("fixture-1", "IMOEX", 86400, Imoex);
  Market.set_benchmark_ticker("IMOEX");

  pb::ReplaySessionResponse Mid;
  ASSERT_TRUE(replay(4, -1, &Mid, false, true).ok());
  ASSERT_TRUE(Mid.report().metrics().has_benchmark_return());
  // Шаги 0…4: open первого дня 1000, close пятого дня 1045.
  EXPECT_NEAR(Mid.report().metrics().benchmark_return(), 1045.0 / 1000.0 - 1.0,
              1e-12);

  // Кривая бенчмарка: капитал 1 000 000 в индексе, точка на каждый пройденный
  // день, не дальше курсора.
  ASSERT_EQ(Mid.report().benchmark_equity_size(), 5);
  EXPECT_EQ(Mid.report().benchmark_equity(0).step(), 0);
  EXPECT_EQ(Mid.report().benchmark_equity(0).equity().units(), 1005000);
  EXPECT_EQ(Mid.report().benchmark_equity(4).step(), 4);
  EXPECT_EQ(Mid.report().benchmark_equity(4).equity().units(), 1045000);

  pb::ReplaySessionResponse Full;
  ASSERT_TRUE(replay(9, -1, &Full, false, true).ok());
  EXPECT_NEAR(Full.report().metrics().benchmark_return(), 1095.0 / 1000.0 - 1.0,
              1e-12);
  ASSERT_EQ(Full.report().benchmark_equity_size(), 10);
  EXPECT_EQ(Full.report().benchmark_equity(9).equity().units(), 1095000);

  Market.set_benchmark_ticker("NOSUCH");
  pb::ReplaySessionResponse Missing;
  ASSERT_TRUE(replay(9, -1, &Missing, false, true).ok());
  EXPECT_FALSE(Missing.report().metrics().has_benchmark_return());
  EXPECT_EQ(Missing.report().benchmark_equity_size(), 0);
}

TEST_F(ReplaySession, BadRequestsAreInvalidArgument) {
  pb::ReplaySessionResponse Response;
  EXPECT_EQ(replay(10, -1, &Response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT); // шагов 10: 0…9
  EXPECT_EQ(replay(-1, -1, &Response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  Journal = {marketOrder(0, 0, "Z", pb::BUY, 1)}; // нет такой бумаги
  EXPECT_EQ(replay(3, -1, &Response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  Journal = {
      marketOrder(0, 5, "A", pb::BUY, 1)}; // шаг записи больше up_to_step
  EXPECT_EQ(replay(3, -1, &Response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  Journal = {pb::JournalEntry()}; // команда не задана
  EXPECT_EQ(replay(3, -1, &Response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  Journal.clear();
  Market.mutable_initial_cash()->set_units(0);
  EXPECT_EQ(replay(3, -1, &Response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(Response.total_steps(), 0); // при ошибке ответ не тронут
}

TEST_F(ReplaySession, NoDataIsFailedPrecondition) {
  Market.set_snapshot_id("нет-такого");
  pb::ReplaySessionResponse Response;
  EXPECT_EQ(replay(3, -1, &Response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
}

// Пары FIFO попадают в отчёт: ключ бумаги, шаги, лоты, цены, P&L до и после
// издержек сходятся с покупкой и продажей из ответа.
TEST_F(ReplaySession, ReportRoundTrips) {
  Journal = {marketOrder(0, 0, "A", pb::BUY, 5),
             marketOrder(1, 4, "A", pb::SELL, 5)};
  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(replay(9, -1, &Response, false, true).ok());
  ASSERT_EQ(Response.fills_size(), 2);
  const pb::Report &Report = Response.report();
  ASSERT_EQ(Report.round_trips_size(), 1);
  EXPECT_EQ(Report.metrics().round_trips(), 1);
  const pb::RoundTrip &T = Report.round_trips(0);
  const pb::Fill &Buy = Response.fills(0);
  const pb::Fill &Sell = Response.fills(1);
  EXPECT_EQ(T.instrument_key(), Buy.instrument_key());
  EXPECT_EQ(T.entry_step(), Buy.step());
  EXPECT_EQ(T.exit_step(), Sell.step());
  EXPECT_EQ(T.lots(), 5);
  EXPECT_EQ(T.entry_price().SerializeAsString(),
            Buy.price().SerializeAsString());
  EXPECT_EQ(T.exit_price().SerializeAsString(),
            Sell.price().SerializeAsString());
  const auto Nanos = [](const pb::Decimal &D) {
    return D.units() * 1'000'000'000LL + D.nano();
  };
  EXPECT_EQ(Nanos(T.gross_pnl()), Nanos(Sell.value()) - Nanos(Buy.value()));
  EXPECT_EQ(Nanos(T.net_pnl()), Nanos(T.gross_pnl()) - Nanos(T.costs()));
  EXPECT_EQ(Nanos(T.costs()),
            Nanos(Buy.commission()) + Nanos(Buy.exchange_fee()) +
                Nanos(Sell.commission()) + Nanos(Sell.exchange_fee()));
}

// INV-4: один и тот же запрос даёт побайтно одинаковый ответ.
TEST_F(ReplaySession, Deterministic) {
  Journal = {marketOrder(0, 0, "A", pb::BUY, 5),
             marketOrder(1, 4, "A", pb::SELL, 5)};
  pb::ReplaySessionResponse A;
  pb::ReplaySessionResponse B;
  ASSERT_TRUE(replay(9, 0, &A, true, true).ok());
  ASSERT_TRUE(replay(9, 0, &B, true, true).ok());
  EXPECT_EQ(A.SerializeAsString(), B.SerializeAsString());
}

// Курсор посреди дня: дневной бар бенчмарка этого дня содержит будущее, поэтому
// отчёт берёт последний полностью пройденный день (INV-1). Минутные бары, два
// торговых дня по три шага.
TEST(ReplaySessionIntraday, BenchmarkStopsAtPreviousFullDay) {
  constexpr std::int64_t kDayMs = 86'400'000;
  constexpr std::int64_t kMoscowOffsetMs = 3 * 3'600'000;
  const std::int64_t Day1 =
      19731 * kDayMs - kMoscowOffsetMs;    // 2024-01-09 00:00 по Москве
  const std::int64_t Day2 = Day1 + kDayMs; // 2024-01-10
  const auto Bar = [](std::int64_t Ts, std::int64_t IntervalMs,
                      const char *Open, const char *Close) {
    core::Bar B;
    B.TsOpenMs = Ts;
    B.AvailableAtMs = Ts + IntervalMs;
    B.Open = B.High = B.Low = core::Decimal::parse(Open);
    B.Close = core::Decimal::parse(Close);
    B.High = std::max(B.Open, B.Close);
    B.Low = std::min(B.Open, B.Close);
    B.Volume = 1000;
    return B;
  };

  core::InMemoryBarSource Bars;
  core::InMemoryCalendarSource Calendar;
  Bars.addSecurity("intraday",
                   core::Security{"FIXA", "", 1, core::Decimal::parse("0.01")});
  Bars.addSecurity(
      "intraday", core::Security{"IMOEX", "", 1, core::Decimal::parse("0.01")});
  std::vector<core::Bar> Minute;
  for (const std::int64_t Day : {Day1, Day2}) {
    for (int I = 0; I < 3; ++I)
      Minute.push_back(Bar(Day + (10 * 60 + I) * 60'000, 60'000, "100", "100"));
  }
  Bars.addSeries("intraday", "FIXA", 60, Minute);
  Bars.addSeries(
      "intraday", "IMOEX", 86400,
      {Bar(Day1, kDayMs, "1000", "1005"), Bar(Day2, kDayMs, "1010", "1015")});
  Calendar.add("intraday", {{20240109, true}, {20240110, true}});
  core::InMemoryRuoniaSource Ruonia;
  Ruonia.add("intraday",
             {core::DailyRate{20240108, core::Decimal::parse("15.0")}});

  pb::MarketSpec Market = fixtureMarket();
  Market.set_snapshot_id("intraday");
  Market.clear_instruments();
  pb::Instrument *A = Market.add_instruments();
  A->set_key("A");
  A->set_ticker("FIXA");
  A->set_lot_size(1);
  *A->mutable_price_step() = dec("0.01");
  Market.set_timeframe(pb::M1);
  Market.set_to_date("2024-01-10");
  Market.set_benchmark_ticker("IMOEX");

  const auto Report = [&](std::int64_t UpTo) {
    worker::Context Context{Bars, Calendar, Ruonia};
    pb::ReplaySessionRequest Request;
    *Request.mutable_market() = Market;
    Request.set_up_to_step(UpTo);
    Request.set_events_from_step(-1);
    Request.set_include_report(true);
    pb::ReplaySessionResponse Response;
    EXPECT_TRUE(worker::handleReplaySession(Context, Request, &Response).ok());
    return Response;
  };

  // Шаг 4 - середина второго дня: бенчмарк только по первому дню (close 1005,
  // не 1015 и не open второго дня).
  const pb::ReplaySessionResponse Mid = Report(4);
  EXPECT_EQ(Mid.total_steps(), 6);
  ASSERT_TRUE(Mid.report().metrics().has_benchmark_return());
  EXPECT_NEAR(Mid.report().metrics().benchmark_return(), 1005.0 / 1000.0 - 1.0,
              1e-12);

  // Шаг 2 - последний шаг первого дня: день пройден полностью.
  const pb::ReplaySessionResponse EndOfDay = Report(2);
  ASSERT_TRUE(EndOfDay.report().metrics().has_benchmark_return());
  EXPECT_NEAR(EndOfDay.report().metrics().benchmark_return(),
              1005.0 / 1000.0 - 1.0, 1e-12);

  // Шаг 1 - первый день ещё идёт, полностью пройденных дней нет: бенчмарка нет.
  EXPECT_FALSE(Report(1).report().metrics().has_benchmark_return());

  // Конец периода: оба дня.
  const pb::ReplaySessionResponse End = Report(5);
  EXPECT_NEAR(End.report().metrics().benchmark_return(), 1015.0 / 1000.0 - 1.0,
              1e-12);
}

// Прогрев: history начинается с баров истории до from_date, журнал и шаги
// сессии не меняются.
TEST_F(ReplaySession, WarmupBarsComeFirstInHistory) {
  Market.set_from_date("2024-01-15");
  Market.set_warmup_bars(2);
  Journal = {marketOrder(0, 0, "A", pb::BUY, 5)};
  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(replay(1, 0, &Response).ok());
  EXPECT_EQ(Response.total_steps(), 6);
  EXPECT_EQ(Response.warmup_steps(), 2);
  // 11 января (A, B), 12 января (A), шаги 0 и 1 (A, B).
  ASSERT_EQ(Response.history_size(), 7);
  EXPECT_EQ(Response.history(0).step(), -2);
  EXPECT_EQ(Response.history(2).step(), -1);
  EXPECT_EQ(Response.history(3).step(), 0);
  // Заявка исполнилась по open шага 1, как и без прогрева (INV-2); событий до
  // шага 0 нет.
  ASSERT_EQ(Response.fills_size(), 1);
  EXPECT_EQ(Response.fills(0).step(), 1);
  for (const pb::StepEvents &E : Response.events())
    EXPECT_GE(E.step(), 1);
}

// Испорченный бар индекса (open первого дня 0): отчёт считается без доходности
// и кривой бенчмарка, а не падает.
TEST_F(ReplaySession, BadBenchmarkOpenDoesNotBreakReport) {
  std::vector<core::Bar> Imoex = Fixture.Bars.load(core::BarRequest{
      "fixture-1", "FIXA", 86400, 0, std::numeric_limits<std::int64_t>::max()});
  Imoex[0].Open = core::Decimal::zero();
  Fixture.Bars.addSecurity(
      "fixture-1",
      core::Security{"IMOEX", "", 1, core::Decimal::parse("0.01")});
  Fixture.Bars.addSeries("fixture-1", "IMOEX", 86400, Imoex);
  Market.set_benchmark_ticker("IMOEX");
  pb::ReplaySessionResponse Response;
  const grpc::Status Status = replay(9, -1, &Response, false, true);
  ASSERT_TRUE(Status.ok()) << Status.error_message();
  EXPECT_FALSE(Response.report().metrics().has_benchmark_return());
  EXPECT_EQ(Response.report().benchmark_equity_size(), 0);
  EXPECT_EQ(Response.report().equity_size(), 10);
}

TEST_F(ReplaySession, ReportIncludesSharpeAndSortinoWithRuonia) {
  Journal = {marketOrder(0, 0, "A", pb::BUY, 500)};
  pb::ReplaySessionResponse Response;
  const grpc::Status Status = replay(9, -1, &Response, false, true);
  ASSERT_TRUE(Status.ok()) << Status.error_message();
  const pb::Metrics &M = Response.report().metrics();
  EXPECT_EQ(M.trading_days(), 10);
  EXPECT_TRUE(M.has_volatility());
  EXPECT_TRUE(M.has_sharpe());
  EXPECT_TRUE(M.has_sortino());
}

TEST_F(ReplaySession, MissingRuoniaThrowsPrecondition) {
  // Подменяем источник RUONIA на пустой без fixture-1
  core::InMemoryRuoniaSource EmptyRuonia;
  worker::Context Context{Fixture.Bars, Fixture.Calendar, EmptyRuonia};
  pb::ReplaySessionRequest Request;
  *Request.mutable_market() = Market;
  Request.set_up_to_step(9);
  Request.set_events_from_step(-1);
  Request.set_include_report(true);
  pb::ReplaySessionResponse Response;
  const grpc::Status Status =
      worker::handleReplaySession(Context, Request, &Response);
  EXPECT_FALSE(Status.ok());
  EXPECT_EQ(Status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

namespace {

class ThrowingRuonia final : public core::RuoniaSource {
public:
  std::vector<core::DailyRate> ruoniaRates(const std::string &,
                                           std::int32_t) override {
    throw core::MarketDataError("ClickHouse: неверная ставка RUONIA: abc");
  }
};

} // namespace

TEST_F(ReplaySession, RuoniaSourceDataErrorIsInternal) {
  ThrowingRuonia Ruonia;
  worker::Context Context{Fixture.Bars, Fixture.Calendar, Ruonia};
  pb::ReplaySessionRequest Request;
  *Request.mutable_market() = Market;
  Request.set_up_to_step(9);
  Request.set_events_from_step(-1);
  Request.set_include_report(true);
  pb::ReplaySessionResponse Response;
  const grpc::Status Status =
      worker::handleReplaySession(Context, Request, &Response);
  EXPECT_EQ(Status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(Status.error_message().find("ClickHouse"), std::string::npos);
}

TEST_F(ReplaySession, PreviousTradingDayFromCalendarWithoutWarmup) {
  Market.set_from_date("2024-01-15"); // понедельник, прогрева нет
  worker::Context Context{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};
  const auto Timeline = worker::loadTimeline(Context, Market);
  ASSERT_EQ(Timeline->warmupSteps(), 0);
  EXPECT_EQ(worker::previousTradingDay(Context, Market, *Timeline), 20240112);
}

TEST_F(ReplaySession, PreviousTradingDayZeroWithoutEarlierDay) {
  // Календарь fixture-1 начинается 8 января, торговых дней раньше 9-го нет.
  worker::Context Context{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};
  const auto Timeline = worker::loadTimeline(Context, Market);
  EXPECT_EQ(worker::previousTradingDay(Context, Market, *Timeline), 0);
}

TEST_F(ReplaySession, PreviousTradingDayPrefersCalendarOverWarmup) {
  // В календаре 12 января не торговый: предыдущий день - 11-е, хотя бар
  // прогрева есть и за 12-е.
  core::InMemoryCalendarSource Calendar;
  std::vector<core::TradingDay> Days;
  for (const core::TradingDay &D :
       Fixture.Calendar.tradingDays(Fixture.SnapshotId, 20240101, 20240131))
    Days.push_back({D.Yyyymmdd, D.Yyyymmdd == 20240112 ? false : D.IsTrading});
  Calendar.add(Fixture.SnapshotId, Days);
  Market.set_from_date("2024-01-15");
  Market.set_warmup_bars(1);
  worker::Context Context{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};
  const auto Timeline = worker::loadTimeline(Context, Market);
  ASSERT_EQ(Timeline->previousDayYmd(), 20240112);
  worker::Context Custom{Fixture.Bars, Calendar, Fixture.Ruonia};
  EXPECT_EQ(worker::previousTradingDay(Custom, Market, *Timeline), 20240111);
}

TEST_F(ReplaySession, PreviousTradingDayFindsDayUpToOneYearBack) {
  // Торговый день за 300 календарных дней до 2024-01-09 (2023-03-15):
  // больше прежнего предела 248, но в пределах года (366 дней).
  core::InMemoryCalendarSource Calendar;
  std::vector<core::TradingDay> Days =
      Fixture.Calendar.tradingDays(Fixture.SnapshotId, 20240101, 20240131);
  Days.push_back({20230315, true});
  Calendar.add(Fixture.SnapshotId, Days);

  worker::Context BaseContext{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};
  const auto Timeline = worker::loadTimeline(BaseContext, Market);
  worker::Context Custom{Fixture.Bars, Calendar, Fixture.Ruonia};
  EXPECT_EQ(worker::previousTradingDay(Custom, Market, *Timeline), 20230315);
}

TEST_F(ReplaySession, RejectsBlindPricesInJournal) {
  pb::JournalEntry BlindPlace;
  BlindPlace.set_seq(0);
  BlindPlace.set_step(0);
  pb::OrderSpec *Spec = BlindPlace.mutable_place()->mutable_order();
  Spec->set_instrument_key("A");
  Spec->set_side(pb::BUY);
  Spec->set_type(pb::LIMIT);
  Spec->set_lots(1);
  Spec->set_time_in_force(pb::GTC);
  Spec->set_is_blind_price(true);
  *Spec->mutable_limit_price() = dec("95.00");

  // Активная рублёвая покупка на шаге 0 -> на шаге 1 есть позиция для
  // OCO-продажи.
  pb::JournalEntry RublePlace;
  RublePlace.set_seq(1);
  RublePlace.set_step(0);
  pb::OrderSpec *RubleSpec = RublePlace.mutable_place()->mutable_order();
  RubleSpec->set_instrument_key("A");
  RubleSpec->set_side(pb::BUY);
  RubleSpec->set_type(pb::MARKET);
  RubleSpec->set_lots(1);
  RubleSpec->set_time_in_force(pb::GTC);

  // Замена с is_blind_price = true -> отклонена целиком, старая заявка активна.
  pb::JournalEntry RubleLimit;
  RubleLimit.set_seq(2);
  RubleLimit.set_step(0);
  pb::OrderSpec *LimitSpec = RubleLimit.mutable_place()->mutable_order();
  LimitSpec->set_instrument_key("A");
  LimitSpec->set_side(pb::BUY);
  LimitSpec->set_type(pb::LIMIT);
  LimitSpec->set_lots(1);
  LimitSpec->set_time_in_force(pb::GTC);
  *LimitSpec->mutable_limit_price() = dec("90.00");

  pb::JournalEntry BlindReplace;
  BlindReplace.set_seq(3);
  BlindReplace.set_step(0);
  BlindReplace.mutable_replace()->set_order_id(4);
  *BlindReplace.mutable_replace()->mutable_replacement()->mutable_order() =
      *Spec;

  // OCO на шаге 1 с is_blind_price = true во второй ноге -> также отклонён.
  pb::JournalEntry BlindOco;
  BlindOco.set_seq(4);
  BlindOco.set_step(1);
  pb::OrderSpec *MainLeg = BlindOco.mutable_place()->mutable_order();
  MainLeg->set_instrument_key("A");
  MainLeg->set_side(pb::SELL);
  MainLeg->set_type(pb::STOP);
  MainLeg->set_lots(1);
  MainLeg->set_time_in_force(pb::GTC);
  *MainLeg->mutable_stop_price() = dec("95.00");
  pb::OrderSpec *TakeLeg = BlindOco.mutable_place()->mutable_oco_with();
  TakeLeg->set_instrument_key("A");
  TakeLeg->set_side(pb::SELL);
  TakeLeg->set_type(pb::TAKE_PROFIT);
  TakeLeg->set_lots(1);
  TakeLeg->set_time_in_force(pb::GTC);
  TakeLeg->set_is_blind_price(true);
  *TakeLeg->mutable_limit_price() = dec("105.00");

  Journal = {BlindPlace, RublePlace, RubleLimit, BlindReplace, BlindOco};
  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(replay(2, -1, &Response, false, false).ok());
  ASSERT_EQ(Response.command_results_size(), 5);

  EXPECT_FALSE(Response.command_results(0).accepted());
  EXPECT_EQ(Response.command_results(0).reason(), pb::INVALID_PARAMS);
  EXPECT_EQ(Response.command_results(0).invalid_field(), "isBlindPrice");
  EXPECT_EQ(Response.orders(0).status(), pb::REJECTED);

  EXPECT_TRUE(Response.command_results(1).accepted());
  EXPECT_TRUE(Response.command_results(2).accepted());
  EXPECT_EQ(Response.orders(2).status(), pb::ACTIVE);

  EXPECT_FALSE(Response.command_results(3).accepted());
  EXPECT_EQ(Response.command_results(3).reason(), pb::INVALID_PARAMS);
  EXPECT_EQ(Response.command_results(3).invalid_field(), "isBlindPrice");
  EXPECT_EQ(Response.orders(2).status(), pb::ACTIVE);

  EXPECT_FALSE(Response.command_results(4).accepted());
  EXPECT_EQ(Response.command_results(4).reason(), pb::INVALID_PARAMS);
  EXPECT_EQ(Response.command_results(4).invalid_field(), "isBlindPrice");
}

TEST_F(ReplaySession, BlindEstimateThenReplayWithP0Not100BuySellOcoAndReplace) {
  const std::int32_t IntervalSec = core::intervalSeconds(core::Timeframe::D1);
  std::vector<core::Bar> BarsA = Fixture.Bars.load(
      core::BarRequest{"fixture-1", "FIXA", IntervalSec, 0, 2000000000000LL});
  // P0 = 200.00 != 100; сдвигаем цены, чтобы заявка на покупку исполнилась.
  BarsA[0].Open = core::Decimal::parse("200.00");
  BarsA[0].Close = core::Decimal::parse("200.00");
  BarsA[1].Open = core::Decimal::parse("195.00");
  BarsA[1].Low = core::Decimal::parse("190.00");
  BarsA[1].Close = core::Decimal::parse("200.00");
  Fixture.Bars.addSeries("fixture-1", "FIXA", IntervalSec, BarsA);

  worker::Context Ctx{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};
  auto Resolve = [&](std::int64_t UpToStep, const pb::OrderSpec &BlindSpec) {
    pb::EstimateOrderRequest Req;
    *Req.mutable_market() = Market;
    for (const pb::JournalEntry &J : Journal)
      *Req.add_journal() = J;
    Req.set_up_to_step(UpToStep);
    *Req.mutable_order() = BlindSpec;
    pb::EstimateOrderResponse Resp;
    EXPECT_TRUE(worker::handleEstimateOrder(Ctx, Req, &Resp).ok());
    EXPECT_TRUE(Resp.has_resolved_order());
    EXPECT_FALSE(Resp.resolved_order().is_blind_price());
    return Resp.resolved_order();
  };

  // 1. Покупка (лимит 98.003 -> 196.006 -> CEILING 196.01), исполнится на
  // шаге 1.
  pb::OrderSpec BlindBuy;
  BlindBuy.set_instrument_key("A");
  BlindBuy.set_side(pb::BUY);
  BlindBuy.set_type(pb::LIMIT);
  BlindBuy.set_lots(2);
  BlindBuy.set_time_in_force(pb::GTC);
  BlindBuy.set_is_blind_price(true);
  *BlindBuy.mutable_limit_price() = dec("98.003");
  const pb::OrderSpec ResolvedBuy = Resolve(0, BlindBuy);
  EXPECT_EQ(core::fromProto(ResolvedBuy.limit_price()),
            core::Decimal::parse("196.01"));

  pb::JournalEntry E0;
  E0.set_seq(0);
  E0.set_step(0);
  *E0.mutable_place()->mutable_order() = ResolvedBuy;
  Journal.push_back(E0);

  // 2. На шаге 1 позиция открыта -> OCO (Стоп 95.003 -> 190.00 FLOOR + Тейк
  // 105.003 -> 210.00 FLOOR).
  pb::OrderSpec BlindStop;
  BlindStop.set_instrument_key("A");
  BlindStop.set_side(pb::SELL);
  BlindStop.set_type(pb::STOP);
  BlindStop.set_lots(1);
  BlindStop.set_time_in_force(pb::GTC);
  BlindStop.set_is_blind_price(true);
  *BlindStop.mutable_stop_price() = dec("95.003");
  const pb::OrderSpec ResolvedStop = Resolve(1, BlindStop);
  EXPECT_EQ(core::fromProto(ResolvedStop.stop_price()),
            core::Decimal::parse("190.00"));

  pb::OrderSpec BlindTake;
  BlindTake.set_instrument_key("A");
  BlindTake.set_side(pb::SELL);
  BlindTake.set_type(pb::TAKE_PROFIT);
  BlindTake.set_lots(1);
  BlindTake.set_time_in_force(pb::GTC);
  BlindTake.set_is_blind_price(true);
  *BlindTake.mutable_limit_price() = dec("105.003");
  const pb::OrderSpec ResolvedTake = Resolve(1, BlindTake);
  EXPECT_EQ(core::fromProto(ResolvedTake.limit_price()),
            core::Decimal::parse("210.00"));

  pb::JournalEntry E1;
  E1.set_seq(1);
  E1.set_step(1);
  *E1.mutable_place()->mutable_order() = ResolvedStop;
  *E1.mutable_place()->mutable_oco_with() = ResolvedTake;
  Journal.push_back(E1);

  // 3. На шаге 1 же заменяем OCO-пару (id = 2) новыми слепыми ценами.
  *BlindStop.mutable_stop_price() = dec("94.003");   // 188.006 -> 188.00
  *BlindTake.mutable_limit_price() = dec("106.003"); // 212.006 -> 212.00
  pb::JournalEntry E2;
  E2.set_seq(2);
  E2.set_step(1);
  E2.mutable_replace()->set_order_id(2);
  *E2.mutable_replace()->mutable_replacement()->mutable_order() =
      Resolve(1, BlindStop);
  *E2.mutable_replace()->mutable_replacement()->mutable_oco_with() =
      Resolve(1, BlindTake);
  Journal.push_back(E2);

  pb::ReplaySessionResponse R1;
  pb::ReplaySessionResponse R2;
  ASSERT_TRUE(replay(2, -1, &R1, false, false).ok());
  ASSERT_TRUE(replay(2, -1, &R2, false, false).ok());
  EXPECT_EQ(R1.SerializeAsString(), R2.SerializeAsString());
  ASSERT_EQ(R1.command_results_size(), 3);
  EXPECT_TRUE(R1.command_results(0).accepted());
  EXPECT_TRUE(R1.command_results(1).accepted());
  EXPECT_TRUE(R1.command_results(2).accepted());
  ASSERT_EQ(R1.orders_size(), 5);
  EXPECT_EQ(core::fromProto(R1.orders(3).spec().stop_price()),
            core::Decimal::parse("188.00"));
  EXPECT_EQ(core::fromProto(R1.orders(4).spec().limit_price()),
            core::Decimal::parse("212.00"));
}

// core#54: производные величины в ответе.
TEST_F(ReplaySession, DerivedFillCostsAndSeq) {
  Journal = {marketOrder(0, 0, "A", pb::BUY, 5),
             marketOrder(1, 4, "A", pb::SELL, 5)};
  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(replay(9, 0, &Response, false, true).ok());
  ASSERT_EQ(Response.fills_size(), 2);
  const core::Decimal Cash = core::fromProto(Market.initial_cash());
  for (int I = 0; I < 2; ++I) {
    const pb::Fill &F = Response.fills(I);
    EXPECT_EQ(F.seq(), I);
    const core::Decimal Cost =
        core::fromProto(F.commission()) + core::fromProto(F.exchange_fee()) +
        core::fromProto(F.spread_cost()) + core::fromProto(F.slippage_cost());
    EXPECT_EQ(core::fromProto(F.cost()), Cost);
    EXPECT_EQ(core::fromProto(F.cost_pct()),
              core::toPercentOfCapital(Cost, Cash));
    EXPECT_EQ(core::fromProto(F.amount_pct()),
              core::toPercentOfCapital(core::fromProto(F.value()), Cash));
  }
  // seq в событиях совпадает с seq в общем списке
  int Seen = 0;
  for (const pb::StepEvents &E : Response.events())
    for (const pb::Fill &F : E.fills())
      EXPECT_EQ(F.seq(), Seen++);
  EXPECT_EQ(Seen, 2);

  const pb::Report &R = Response.report();
  EXPECT_EQ(core::fromProto(R.costs().total()),
            core::fromProto(R.costs().commission()) +
                core::fromProto(R.costs().exchange_fee()) +
                core::fromProto(R.costs().spread()) +
                core::fromProto(R.costs().slippage()));
  ASSERT_EQ(R.round_trips_size(), 1);
  EXPECT_EQ(R.round_trips(0).seq(), 0);
  // кривая просадки - по точке на точку кривой капитала
  ASSERT_EQ(R.underwater_size(), R.equity_size());
  for (int I = 0; I < R.equity_size(); ++I) {
    EXPECT_EQ(R.underwater(I).step(), R.equity(I).step());
    EXPECT_GE(core::fromProto(R.underwater(I).drawdown_pct()),
              core::Decimal::zero());
  }
}

// Нет сделок: нули, а не отсутствие полей.
TEST_F(ReplaySession, DerivedNoTrades) {
  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(replay(5, -1, &Response, false, true).ok());
  EXPECT_EQ(Response.fills_size(), 0);
  EXPECT_EQ(core::fromProto(Response.report().costs().total()),
            core::Decimal::zero());
  EXPECT_EQ(Response.report().underwater_size(),
            Response.report().equity_size());
}

// Номер дня курсора отдаётся без истории и отчёта и совпадает с trading_day
// бара на курсоре и с Metrics.trading_days.
TEST_F(ReplaySession, CurrentDayWithoutHistoryAndReport) {
  pb::ReplaySessionResponse Full;
  ASSERT_TRUE(replay(7, -1, &Full, true, true).ok());
  pb::ReplaySessionResponse Bare;
  ASSERT_TRUE(replay(7, -1, &Bare, false, false).ok());
  EXPECT_GE(Bare.current_day(), 1);
  std::int32_t LastDay = 0;
  for (const pb::Bar &B : Full.history())
    if (B.step() == Full.last_step())
      LastDay = B.trading_day();
  EXPECT_EQ(Bare.current_day(), LastDay);
  EXPECT_EQ(Bare.current_day(), Full.report().metrics().trading_days());
}

// Слепой вид: cost_pct от рублёвой суммы, total_pct, underwater, seq.
TEST_F(ReplaySession, DerivedBlindView) {
  Journal = {marketOrder(0, 0, "A", pb::BUY, 5),
             marketOrder(1, 4, "A", pb::SELL, 5)};
  worker::Context Context{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};
  pb::ReplaySessionRequest Request;
  *Request.mutable_market() = Market;
  for (const pb::JournalEntry &E : Journal)
    *Request.add_journal() = E;
  Request.set_up_to_step(9);
  Request.set_events_from_step(-1);
  Request.set_include_report(true);
  Request.set_blind(true);
  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(worker::handleReplaySession(Context, Request, &Response).ok());
  const core::Decimal Cash = core::fromProto(Market.initial_cash());
  const pb::BlindSessionView &Bv = Response.blind_view();
  ASSERT_EQ(Bv.fills_size(), Response.fills_size());
  for (int I = 0; I < Bv.fills_size(); ++I) {
    EXPECT_EQ(Bv.fills(I).seq(), Response.fills(I).seq());
    EXPECT_EQ(core::fromProto(Bv.fills(I).cost_pct()),
              core::toPercentOfCapital(
                  core::fromProto(Response.fills(I).cost()), Cash));
  }
  EXPECT_EQ(core::fromProto(Bv.report().costs().total_pct()),
            core::toPercentOfCapital(
                core::fromProto(Response.report().costs().total()), Cash));
  EXPECT_EQ(Bv.report().underwater_size(), Response.report().underwater_size());
  ASSERT_EQ(Bv.report().round_trips_size(), 1);
  EXPECT_EQ(Bv.report().round_trips(0).seq(), 0);
}

// core#54: проценты портфеля, позиций, изменение цены, просадка в днях.
// База всех процентов - начальный капитал.
TEST_F(ReplaySession, DerivedPortfolioAndPositionPcts) {
  Journal = {marketOrder(0, 0, "A", pb::BUY, 5)};
  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(replay(4, 0, &Response, false, true).ok());
  const core::Decimal Cash = core::fromProto(Market.initial_cash());
  const pb::Portfolio &P = Response.portfolio();
  const core::Decimal Equity = core::fromProto(P.equity());
  const core::Decimal Cost = core::fromProto(Response.fills(0).cost());
  EXPECT_EQ(core::fromProto(P.costs_pct()),
            core::toPercentOfCapital(Cost, Cash));
  EXPECT_EQ(core::fromProto(P.costs()), Cost);
  EXPECT_EQ(core::fromProto(P.pnl_pct()),
            core::toPercentOfCapital(Equity - Cash, Cash));
  EXPECT_EQ(core::fromProto(P.pnl()), Equity - Cash);
  EXPECT_EQ(core::fromProto(P.equity_pct()),
            core::toPercentOfCapital(Equity, Cash));
  EXPECT_EQ(core::fromProto(P.cash_pct()),
            core::toPercentOfCapital(core::fromProto(P.cash()), Cash));
  ASSERT_EQ(P.positions_size(), 1);
  const pb::Position &Pos = P.positions(0);
  const core::Decimal Value = core::fromProto(Pos.market_value());
  EXPECT_EQ(core::fromProto(P.invested()), Value);
  EXPECT_EQ(core::fromProto(P.invested_pct()),
            core::toPercentOfCapital(Value, Cash));
  EXPECT_EQ(core::fromProto(Pos.value_pct()),
            core::fromProto(P.invested_pct()));
  // cash_pct + invested_pct = equity_pct с точностью до 0,01
  const core::Decimal Diff =
      core::fromProto(P.equity_pct()) -
      (core::fromProto(P.cash_pct()) + core::fromProto(P.invested_pct()));
  EXPECT_LE(Diff.abs(), core::Decimal::parse("0.01"));
  // себестоимость - точная сумма покупки (value сделки)
  EXPECT_EQ(core::fromProto(Pos.pnl_pct()),
            core::toPercentOfCapital(
                Value - core::fromProto(Response.fills(0).value()), Cash));
  // портфель в событии шага 4 - тот же; покупка исполнена на шаге 1
  EXPECT_EQ(core::fromProto(Response.events(3).portfolio().costs_pct()),
            core::fromProto(P.costs_pct()));
  EXPECT_EQ(core::fromProto(Response.events(0).portfolio().costs_pct()),
            core::toPercentOfCapital(Cost, Cash));
  // пустой портфель: нули, equity 100%
  Journal.clear();
  pb::ReplaySessionResponse Empty;
  ASSERT_TRUE(replay(4, -1, &Empty, false, true).ok());
  EXPECT_EQ(core::fromProto(Empty.portfolio().invested_pct()),
            core::Decimal::zero());
  EXPECT_EQ(core::fromProto(Empty.portfolio().costs_pct()),
            core::Decimal::zero());
  EXPECT_EQ(core::fromProto(Empty.portfolio().pnl_pct()),
            core::Decimal::zero());
  EXPECT_EQ(core::fromProto(Empty.portfolio().equity_pct()),
            core::Decimal::parse("100.00"));
  EXPECT_EQ(core::fromProto(Empty.portfolio().invested()),
            core::Decimal::zero());
}

// seq сделок не зависит от курсора: префикс совпадает (UUID v5 у backend).
TEST_F(ReplaySession, DerivedSeqStableAcrossCursor) {
  Journal = {marketOrder(0, 0, "A", pb::BUY, 5),
             marketOrder(1, 4, "A", pb::SELL, 5)};
  pb::ReplaySessionResponse Early, Late;
  ASSERT_TRUE(replay(9, -1, &Late, false, false).ok());
  Journal.pop_back(); // команды позже курсора журнал не допускает
  ASSERT_TRUE(replay(2, -1, &Early, false, false).ok());
  ASSERT_EQ(Early.fills_size(), 1);
  ASSERT_EQ(Late.fills_size(), 2);
  EXPECT_EQ(Early.fills(0).seq(), Late.fills(0).seq());
  EXPECT_EQ(Late.fills(1).seq(), 1);
}

TEST_F(ReplaySession, DerivedInstrumentChanges) {
  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(replay(3, -1, &Response, false, false).ok());
  ASSERT_EQ(Response.instrument_changes_size(), 2);
  const pb::InstrumentChange &A = Response.instrument_changes(0);
  EXPECT_EQ(A.instrument_key(), "A");
  ASSERT_TRUE(A.has_step_pct());
  EXPECT_EQ(core::fromProto(A.step_pct()), core::Decimal::parse("2.91"));
  EXPECT_EQ(core::fromProto(A.period_pct()), core::Decimal::parse("6.00"));
}

TEST_F(ReplaySession, DerivedMaxDrawdownDays) {
  Journal = {marketOrder(0, 0, "A", pb::BUY, 50),
             marketOrder(1, 4, "A", pb::SELL, 50)};
  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(replay(9, -1, &Response, false, true).ok());
  const pb::Metrics &M = Response.report().metrics();
  // шаг шкалы фикстуры - день, дни равны шагам
  EXPECT_EQ(M.max_drawdown_days(), M.max_drawdown_steps());
}

TEST_F(ReplaySession, DerivedBlindPortfolioAndChange) {
  Journal = {marketOrder(0, 0, "A", pb::BUY, 5)};
  worker::Context Context{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};
  pb::ReplaySessionRequest Request;
  *Request.mutable_market() = Market;
  *Request.add_journal() = Journal[0];
  Request.set_up_to_step(3);
  Request.set_events_from_step(0);
  Request.set_include_report(true);
  Request.set_blind(true);
  pb::ReplaySessionResponse Response;
  ASSERT_TRUE(worker::handleReplaySession(Context, Request, &Response).ok());
  const pb::BlindSessionView &Bv = Response.blind_view();
  EXPECT_EQ(core::fromProto(Bv.portfolio().invested_pct()),
            core::fromProto(Response.portfolio().invested_pct()));
  EXPECT_EQ(core::fromProto(Bv.portfolio().equity_pct()),
            core::fromProto(Response.portfolio().equity_pct()));
  ASSERT_EQ(Bv.portfolio().positions_size(), 1);
  EXPECT_EQ(core::fromProto(Bv.portfolio().positions(0).pnl_pct()),
            core::fromProto(Response.portfolio().positions(0).pnl_pct()));
  EXPECT_EQ(core::fromProto(Bv.portfolio().costs_pct()),
            core::fromProto(Response.portfolio().costs_pct()));
  ASSERT_EQ(Bv.events_size(), 3);
  EXPECT_EQ(core::fromProto(Bv.events(2).portfolio().costs_pct()),
            core::fromProto(Response.events(2).portfolio().costs_pct()));
  ASSERT_EQ(Bv.instruments_size(), 2);
  EXPECT_EQ(core::fromProto(Bv.instruments(0).change_step_pct()),
            core::Decimal::parse("2.91"));
  EXPECT_EQ(core::fromProto(Bv.instruments(0).change_period_pct()),
            core::Decimal::parse("6.00"));
}
