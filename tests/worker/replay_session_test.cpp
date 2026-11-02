#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <string>

#include "core/market/bar.hpp"
#include "worker_test_support.hpp"

namespace {

using namespace worker_test;

class ReplaySession : public ::testing::Test {
 protected:
  grpc::Status replay(std::int64_t upTo, std::int64_t eventsFrom, pb::ReplaySessionResponse* response,
                      bool history = true, bool report = false) {
    worker::Context context{fixture_.bars, fixture_.calendar};
    pb::ReplaySessionRequest request;
    *request.mutable_market() = market_;
    for (const pb::JournalEntry& e : journal_) *request.add_journal() = e;
    request.set_up_to_step(upTo);
    request.set_events_from_step(eventsFrom);
    request.set_include_history(history);
    request.set_include_report(report);
    return worker::handleReplaySession(context, request, response);
  }

  core::testing::Fixture fixture_ = core::testing::fixture1();
  pb::MarketSpec market_ = fixtureMarket();
  std::vector<pb::JournalEntry> journal_;
};

}  // namespace

// INV-1: ни history, ни events не содержат шагов позже up_to_step.
TEST_F(ReplaySession, UpToStepNoFuture) {
  journal_ = {marketOrder(0, 0, "A", pb::BUY, 5)};
  pb::ReplaySessionResponse response;
  ASSERT_TRUE(replay(3, 0, &response).ok());

  EXPECT_EQ(response.total_steps(), 10);
  EXPECT_EQ(response.status(), pb::RUNNING);
  EXPECT_EQ(response.last_step(), 3);

  // История: шаги 0…3, у A бар на каждом, у B нет бара на шаге 3.
  ASSERT_EQ(response.history_size(), 7);
  for (const pb::Bar& b : response.history()) EXPECT_LE(b.step(), 3);
  EXPECT_EQ(response.history(6).step(), 3);

  // События (0; 3]: шаги 1, 2, 3.
  ASSERT_EQ(response.events_size(), 3);
  for (int i = 0; i < response.events_size(); ++i) {
    const pb::StepEvents& e = response.events(i);
    EXPECT_EQ(e.step(), i + 1);
    for (const pb::Bar& b : e.bars()) EXPECT_EQ(b.step(), e.step());
    for (const pb::Fill& f : e.fills()) EXPECT_LE(f.step(), 3);
  }
  for (const pb::Fill& f : response.fills()) EXPECT_LE(f.step(), 3);
  // Покупка исполнилась на открытии шага 1 (INV-2), и событие шага 1 её содержит.
  ASSERT_EQ(response.fills_size(), 1);
  EXPECT_EQ(response.fills(0).step(), 1);
  EXPECT_EQ(response.fills(0).instrument_key(), "A");
  EXPECT_EQ(response.fills(0).side(), pb::BUY);
  EXPECT_EQ(response.fills(0).lots(), 5);
  ASSERT_EQ(response.events(0).fills_size(), 1);

  ASSERT_EQ(response.portfolio().positions_size(), 1);
  EXPECT_EQ(response.portfolio().positions(0).instrument_key(), "A");
  EXPECT_EQ(response.portfolio().positions(0).lots(), 5);
  // Стоимость позиции по close шага 3: 106 * 5 лотов * 10 штук.
  EXPECT_EQ(response.portfolio().positions(0).market_value().units(), 5300);
  EXPECT_FALSE(response.has_report());
}

TEST_F(ReplaySession, EventsFromStepIsExclusive) {
  pb::ReplaySessionResponse response;
  ASSERT_TRUE(replay(5, 3, &response, false).ok());
  ASSERT_EQ(response.events_size(), 2);  // (3; 5]
  EXPECT_EQ(response.events(0).step(), 4);
  EXPECT_EQ(response.events(1).step(), 5);
  EXPECT_EQ(response.history_size(), 0);

  pb::ReplaySessionResponse none;
  ASSERT_TRUE(replay(5, -1, &none, false).ok());
  EXPECT_EQ(none.events_size(), 0);

  pb::ReplaySessionResponse atCursor;
  ASSERT_TRUE(replay(5, 5, &atCursor, false).ok());
  EXPECT_EQ(atCursor.events_size(), 0);
}

TEST_F(ReplaySession, CommandResultsPerEntry) {
  journal_ = {marketOrder(0, 0, "A", pb::BUY, 5),
              marketOrder(1, 1, "A", pb::SELL, 100),  // позиции всего 5 лотов
              marketOrder(2, 2, "A", pb::BUY, 0)};     // неверное количество
  pb::JournalEntry cancel;
  cancel.set_seq(3);
  cancel.set_step(3);
  cancel.mutable_cancel()->set_order_id(0);  // заявка 0 уже исполнена
  journal_.push_back(cancel);

  pb::ReplaySessionResponse response;
  ASSERT_TRUE(replay(4, -1, &response).ok());
  ASSERT_EQ(response.command_results_size(), 4);

  const pb::CommandResult& first = response.command_results(0);
  EXPECT_EQ(first.seq(), 0);
  EXPECT_TRUE(first.accepted());
  ASSERT_EQ(first.order_ids_size(), 1);
  EXPECT_EQ(first.order_ids(0), 0);

  const pb::CommandResult& oversell = response.command_results(1);
  EXPECT_EQ(oversell.seq(), 1);
  EXPECT_FALSE(oversell.accepted());
  EXPECT_EQ(oversell.reason(), pb::INSUFFICIENT_POSITION);
  ASSERT_EQ(oversell.order_ids_size(), 1);
  EXPECT_EQ(oversell.order_ids(0), 2);  // seq 1, нога 0

  const pb::CommandResult& zero = response.command_results(2);
  EXPECT_FALSE(zero.accepted());
  EXPECT_EQ(zero.reason(), pb::INVALID_PARAMS);
  EXPECT_EQ(zero.invalid_field(), "lots");

  const pb::CommandResult& late = response.command_results(3);
  EXPECT_FALSE(late.accepted());
  EXPECT_EQ(late.reason(), pb::ORDER_NOT_ACTIVE);
  EXPECT_EQ(late.order_ids_size(), 0);

  // Отклонённые заявки остаются в журнале заявок со статусом REJECTED.
  ASSERT_EQ(response.orders_size(), 3);
  EXPECT_EQ(response.orders(0).id(), 0);
  EXPECT_EQ(response.orders(0).status(), pb::FILLED);
  EXPECT_EQ(response.orders(0).spec().instrument_key(), "A");
  EXPECT_EQ(response.orders(0).spec().type(), pb::MARKET);
  EXPECT_FALSE(response.orders(0).spec().has_limit_price());
  EXPECT_EQ(response.orders(1).id(), 2);
  EXPECT_EQ(response.orders(1).status(), pb::REJECTED);
  EXPECT_EQ(response.orders(1).reason(), pb::INSUFFICIENT_POSITION);
  EXPECT_EQ(response.orders(1).oco_sibling_id(), -1);
}

TEST_F(ReplaySession, LimitOrderKeepsPrices) {
  pb::JournalEntry e = marketOrder(0, 0, "A", pb::BUY, 2);
  pb::OrderSpec* spec = e.mutable_place()->mutable_order();
  spec->set_type(pb::LIMIT);
  *spec->mutable_limit_price() = dec("90.5");
  journal_ = {e};
  pb::ReplaySessionResponse response;
  ASSERT_TRUE(replay(2, -1, &response, false).ok());
  ASSERT_EQ(response.orders_size(), 1);
  const pb::Order& order = response.orders(0);
  EXPECT_EQ(order.status(), pb::ACTIVE);
  EXPECT_EQ(order.spec().type(), pb::LIMIT);
  ASSERT_TRUE(order.spec().has_limit_price());
  EXPECT_EQ(order.spec().limit_price().units(), 90);
  EXPECT_EQ(order.spec().limit_price().nano(), 500000000);
  EXPECT_FALSE(order.spec().has_stop_price());
}

TEST_F(ReplaySession, OcoPairGetsTwoIds) {
  // Покупка 5 лотов, затем OCO-продажа: стоп ниже рынка и тейк-профит выше.
  journal_ = {marketOrder(0, 0, "A", pb::BUY, 5)};
  pb::JournalEntry oco;
  oco.set_seq(1);
  oco.set_step(1);
  pb::PlaceOrder* place = oco.mutable_place();
  pb::OrderSpec* stop = place->mutable_order();
  stop->set_instrument_key("A");
  stop->set_side(pb::SELL);
  stop->set_type(pb::STOP);
  stop->set_lots(5);
  *stop->mutable_stop_price() = dec("90");
  stop->set_time_in_force(pb::GTC);
  pb::OrderSpec* take = place->mutable_oco_with();
  take->set_instrument_key("A");
  take->set_side(pb::SELL);
  take->set_type(pb::TAKE_PROFIT);
  take->set_lots(5);
  *take->mutable_limit_price() = dec("200");
  take->set_time_in_force(pb::GTC);
  journal_.push_back(oco);

  pb::ReplaySessionResponse response;
  ASSERT_TRUE(replay(2, -1, &response, false).ok());
  ASSERT_EQ(response.command_results_size(), 2);
  const pb::CommandResult& r = response.command_results(1);
  EXPECT_TRUE(r.accepted());
  ASSERT_EQ(r.order_ids_size(), 2);
  EXPECT_EQ(r.order_ids(0), 2);
  EXPECT_EQ(r.order_ids(1), 3);
  ASSERT_EQ(response.orders_size(), 3);
  EXPECT_EQ(response.orders(1).oco_sibling_id(), 3);
  EXPECT_EQ(response.orders(2).oco_sibling_id(), 2);
}

TEST_F(ReplaySession, ReportOnRequest) {
  journal_ = {marketOrder(0, 0, "A", pb::BUY, 5)};
  pb::ReplaySessionResponse response;
  ASSERT_TRUE(replay(9, -1, &response, false, true).ok());
  EXPECT_EQ(response.status(), pb::FINISHED_END);
  EXPECT_EQ(response.last_step(), 9);
  ASSERT_TRUE(response.has_report());
  const pb::Report& report = response.report();

  // final_equity - стоимость портфеля на last_step.
  EXPECT_EQ(report.final_equity().units(), response.portfolio().equity().units());
  EXPECT_EQ(report.final_equity().nano(), response.portfolio().equity().nano());
  ASSERT_EQ(report.equity_size(), 10);
  EXPECT_EQ(report.equity(9).step(), 9);
  EXPECT_EQ(report.equity(9).equity().units(), report.final_equity().units());
  EXPECT_EQ(report.level(), pb::OK);

  const double finalEquity = static_cast<double>(report.final_equity().units()) +
                             static_cast<double>(report.final_equity().nano()) * 1e-9;
  EXPECT_NEAR(report.metrics().total_return(), finalEquity / 1000000.0 - 1.0, 1e-9);
  EXPECT_EQ(report.metrics().fills(), 1);
  EXPECT_EQ(report.metrics().trading_days(), 10);
  EXPECT_FALSE(report.metrics().has_benchmark_return());
  // Комиссия и сбор покупки в итогах издержек.
  EXPECT_GT(report.costs().commission().units() + report.costs().commission().nano(), 0);

  pb::ReplaySessionResponse without;
  ASSERT_TRUE(replay(9, -1, &without, false, false).ok());
  EXPECT_FALSE(without.has_report());
}

// Бенчмарк в отчёте не заглядывает в будущее (INV-1): считается по последний полностью пройденный день.
TEST_F(ReplaySession, BenchmarkStopsAtCursor) {
  std::vector<core::Bar> imoex = fixture_.bars.load(
      core::BarRequest{"fixture-1", "FIXA", 86400, 0, std::numeric_limits<std::int64_t>::max()});
  ASSERT_EQ(imoex.size(), 10U);
  for (std::size_t i = 0; i < imoex.size(); ++i) {
    imoex[i].open = core::Decimal::fromUnits(1000 + 10 * static_cast<std::int64_t>(i));
    imoex[i].close = core::Decimal::fromUnits(1005 + 10 * static_cast<std::int64_t>(i));
  }
  fixture_.bars.addSecurity("fixture-1", core::Security{"IMOEX", "", 1, core::Decimal::parse("0.01")});
  fixture_.bars.addSeries("fixture-1", "IMOEX", 86400, imoex);
  market_.set_benchmark_ticker("IMOEX");

  pb::ReplaySessionResponse mid;
  ASSERT_TRUE(replay(4, -1, &mid, false, true).ok());
  ASSERT_TRUE(mid.report().metrics().has_benchmark_return());
  // Шаги 0…4: open первого дня 1000, close пятого дня 1045.
  EXPECT_NEAR(mid.report().metrics().benchmark_return(), 1045.0 / 1000.0 - 1.0, 1e-12);

  pb::ReplaySessionResponse full;
  ASSERT_TRUE(replay(9, -1, &full, false, true).ok());
  EXPECT_NEAR(full.report().metrics().benchmark_return(), 1095.0 / 1000.0 - 1.0, 1e-12);

  market_.set_benchmark_ticker("NOSUCH");
  pb::ReplaySessionResponse missing;
  ASSERT_TRUE(replay(9, -1, &missing, false, true).ok());
  EXPECT_FALSE(missing.report().metrics().has_benchmark_return());
}

TEST_F(ReplaySession, BadRequestsAreInvalidArgument) {
  pb::ReplaySessionResponse response;
  EXPECT_EQ(replay(10, -1, &response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);  // шагов 10: 0…9
  EXPECT_EQ(replay(-1, -1, &response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  journal_ = {marketOrder(0, 0, "Z", pb::BUY, 1)};  // нет такой бумаги
  EXPECT_EQ(replay(3, -1, &response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  journal_ = {marketOrder(0, 5, "A", pb::BUY, 1)};  // шаг записи больше up_to_step
  EXPECT_EQ(replay(3, -1, &response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  journal_ = {pb::JournalEntry()};  // команда не задана
  EXPECT_EQ(replay(3, -1, &response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  journal_.clear();
  market_.mutable_initial_cash()->set_units(0);
  EXPECT_EQ(replay(3, -1, &response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(response.total_steps(), 0);  // при ошибке ответ не тронут
}

TEST_F(ReplaySession, NoDataIsFailedPrecondition) {
  market_.set_snapshot_id("нет-такого");
  pb::ReplaySessionResponse response;
  EXPECT_EQ(replay(3, -1, &response).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// INV-4: один и тот же запрос даёт побайтно одинаковый ответ.
TEST_F(ReplaySession, Deterministic) {
  journal_ = {marketOrder(0, 0, "A", pb::BUY, 5), marketOrder(1, 4, "A", pb::SELL, 5)};
  pb::ReplaySessionResponse a;
  pb::ReplaySessionResponse b;
  ASSERT_TRUE(replay(9, 0, &a, true, true).ok());
  ASSERT_TRUE(replay(9, 0, &b, true, true).ok());
  EXPECT_EQ(a.SerializeAsString(), b.SerializeAsString());
}

// Курсор посреди дня: дневной бар бенчмарка этого дня содержит будущее, поэтому отчёт берёт последний
// полностью пройденный день (INV-1). Минутные бары, два торговых дня по три шага.
TEST(ReplaySessionIntraday, BenchmarkStopsAtPreviousFullDay) {
  constexpr std::int64_t kDayMs = 86'400'000;
  constexpr std::int64_t kMoscowOffsetMs = 3 * 3'600'000;
  const std::int64_t day1 = 19731 * kDayMs - kMoscowOffsetMs;  // 2024-01-09 00:00 по Москве
  const std::int64_t day2 = day1 + kDayMs;                      // 2024-01-10
  const auto bar = [](std::int64_t ts, std::int64_t intervalMs, const char* open, const char* close) {
    core::Bar b;
    b.tsOpenMs = ts;
    b.availableAtMs = ts + intervalMs;
    b.open = b.high = b.low = core::Decimal::parse(open);
    b.close = core::Decimal::parse(close);
    b.high = std::max(b.open, b.close);
    b.low = std::min(b.open, b.close);
    b.volume = 1000;
    return b;
  };

  core::InMemoryBarSource bars;
  core::InMemoryCalendarSource calendar;
  bars.addSecurity("intraday", core::Security{"FIXA", "", 1, core::Decimal::parse("0.01")});
  bars.addSecurity("intraday", core::Security{"IMOEX", "", 1, core::Decimal::parse("0.01")});
  std::vector<core::Bar> minute;
  for (const std::int64_t day : {day1, day2}) {
    for (int i = 0; i < 3; ++i) minute.push_back(bar(day + (10 * 60 + i) * 60'000, 60'000, "100", "100"));
  }
  bars.addSeries("intraday", "FIXA", 60, minute);
  bars.addSeries("intraday", "IMOEX", 86400,
                 {bar(day1, kDayMs, "1000", "1005"), bar(day2, kDayMs, "1010", "1015")});
  calendar.add("intraday", {{20240109, true}, {20240110, true}});

  pb::MarketSpec market = fixtureMarket();
  market.set_snapshot_id("intraday");
  market.clear_instruments();
  pb::Instrument* a = market.add_instruments();
  a->set_key("A");
  a->set_ticker("FIXA");
  a->set_lot_size(1);
  *a->mutable_price_step() = dec("0.01");
  market.set_timeframe(pb::M1);
  market.set_to_date("2024-01-10");
  market.set_benchmark_ticker("IMOEX");

  const auto report = [&](std::int64_t upTo) {
    worker::Context context{bars, calendar};
    pb::ReplaySessionRequest request;
    *request.mutable_market() = market;
    request.set_up_to_step(upTo);
    request.set_events_from_step(-1);
    request.set_include_report(true);
    pb::ReplaySessionResponse response;
    EXPECT_TRUE(worker::handleReplaySession(context, request, &response).ok());
    return response;
  };

  // Шаг 4 - середина второго дня: бенчмарк только по первому дню (close 1005, не 1015 и не open второго дня).
  const pb::ReplaySessionResponse mid = report(4);
  EXPECT_EQ(mid.total_steps(), 6);
  ASSERT_TRUE(mid.report().metrics().has_benchmark_return());
  EXPECT_NEAR(mid.report().metrics().benchmark_return(), 1005.0 / 1000.0 - 1.0, 1e-12);

  // Шаг 2 - последний шаг первого дня: день пройден полностью.
  const pb::ReplaySessionResponse endOfDay = report(2);
  ASSERT_TRUE(endOfDay.report().metrics().has_benchmark_return());
  EXPECT_NEAR(endOfDay.report().metrics().benchmark_return(), 1005.0 / 1000.0 - 1.0, 1e-12);

  // Шаг 1 - первый день ещё идёт, полностью пройденных дней нет: бенчмарка нет.
  EXPECT_FALSE(report(1).report().metrics().has_benchmark_return());

  // Конец периода: оба дня.
  const pb::ReplaySessionResponse end = report(5);
  EXPECT_NEAR(end.report().metrics().benchmark_return(), 1015.0 / 1000.0 - 1.0, 1e-12);
}
