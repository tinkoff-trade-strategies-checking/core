#include <gtest/gtest.h>

#include <functional>
#include <stdexcept>
#include <string>

#include "clickhouse/clickhouse_client.hpp"
#include "convert/market.hpp"
#include "core/market/bar_source.hpp"
#include "worker_test_support.hpp"

namespace {

using namespace worker_test;

// Источник, который бросает заданное исключение при любом чтении.
class ThrowingBarSource final : public core::BarSource {
public:
  explicit ThrowingBarSource(std::function<void()> Action)
      : Thrower(std::move(Action)) {}
  std::vector<core::Bar> load(const core::BarRequest &) override {
    Thrower();
    return {};
  }
  core::Security security(const std::string &, const std::string &) override {
    Thrower();
    return {};
  }

private:
  std::function<void()> Thrower;
};

class GetBars : public ::testing::Test {
protected:
  grpc::Status get(std::int64_t From, std::int64_t To,
                   pb::GetBarsResponse *Response,
                   pb::MarketSpec Market = fixtureMarket()) {
    worker::Context Context{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};
    pb::GetBarsRequest Request;
    *Request.mutable_market() = std::move(Market);
    Request.set_from_step(From);
    Request.set_to_step(To);
    return worker::handleGetBars(Context, Request, Response);
  }

  core::testing::Fixture Fixture = core::testing::fixture1();
};

} // namespace

TEST_F(GetBars, Range) {
  pb::GetBarsResponse Response;
  ASSERT_TRUE(get(2, 4, &Response).ok());
  EXPECT_EQ(Response.total_steps(), 10);
  // Шаги 2, 3, 4: у A бары на всех, у B нет бара на шаге 3. Порядок: шаг, затем
  // бумага.
  ASSERT_EQ(Response.bars_size(), 5);
  const char *const Keys[] = {"A", "B", "A", "A", "B"};
  const std::int64_t Steps[] = {2, 2, 3, 4, 4};
  for (int I = 0; I < Response.bars_size(); ++I) {
    EXPECT_EQ(Response.bars(I).step(), Steps[I]) << I;
    EXPECT_EQ(Response.bars(I).instrument_key(), Keys[I]) << I;
  }
  const pb::Bar &Bar = Response.bars(0);
  EXPECT_EQ(Bar.close().units(), 103);
  EXPECT_EQ(Bar.close().nano(), 0);
  EXPECT_EQ(Bar.open().units(), 101);
  EXPECT_EQ(Bar.volume_lots(), 10000); // 100000 штук / лот 10
  EXPECT_EQ(Bar.trading_day(), 3);
  EXPECT_FALSE(Bar.after_halt());
  EXPECT_EQ(Response.bars(1).close().units(), 0);
  EXPECT_EQ(Response.bars(1).close().nano(), 20000000); // 0.02
  EXPECT_EQ(Response.bars(1).volume_lots(), 100000); // 1e9 штук / лот 10000
}

TEST_F(GetBars, SingleStepAndWholeRange) {
  pb::GetBarsResponse One;
  ASSERT_TRUE(get(9, 9, &One).ok());
  ASSERT_EQ(One.bars_size(), 2);
  EXPECT_EQ(One.bars(0).step(), 9);

  pb::GetBarsResponse All;
  ASSERT_TRUE(get(0, 9, &All).ok());
  EXPECT_EQ(All.bars_size(), 19); // 10 баров A и 9 баров B
}

TEST_F(GetBars, BarAfterGapIsMarked) {
  // У B нет бара 12 января: следующий бар B (шаг 4) идёт после разрыва торгов.
  pb::GetBarsResponse Response;
  ASSERT_TRUE(get(4, 4, &Response).ok());
  ASSERT_EQ(Response.bars_size(), 2);
  EXPECT_EQ(Response.bars(1).instrument_key(), "B");
  EXPECT_TRUE(Response.bars(1).after_halt());
}

TEST_F(GetBars, BadRangeIsInvalidArgument) {
  pb::GetBarsResponse Response;
  EXPECT_EQ(get(0, 10, &Response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(get(-1, 3, &Response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(get(5, 4, &Response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(Response.bars_size(), 0);
}

TEST_F(GetBars, BadSpecIsInvalidArgument) {
  pb::GetBarsResponse Response;
  pb::MarketSpec BadDate = fixtureMarket();
  BadDate.set_from_date("09.01.2024");
  EXPECT_EQ(get(0, 1, &Response, BadDate).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  pb::MarketSpec NoTimeframe = fixtureMarket();
  NoTimeframe.set_timeframe(pb::TIMEFRAME_UNSPECIFIED);
  EXPECT_EQ(get(0, 1, &Response, NoTimeframe).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  pb::MarketSpec NoInstruments = fixtureMarket();
  NoInstruments.clear_instruments();
  EXPECT_EQ(get(0, 1, &Response, NoInstruments).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(GetBars, NoDataIsFailedPrecondition) {
  pb::GetBarsResponse Response;
  pb::MarketSpec UnknownSnapshot = fixtureMarket();
  UnknownSnapshot.set_snapshot_id("нет-такого");
  EXPECT_EQ(get(0, 1, &Response, UnknownSnapshot).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  pb::MarketSpec UnknownTicker = fixtureMarket();
  UnknownTicker.mutable_instruments(0)->set_ticker("NOPE");
  EXPECT_EQ(get(0, 1, &Response, UnknownTicker).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  pb::MarketSpec EmptyPeriod = fixtureMarket();
  EmptyPeriod.set_from_date("2023-01-01");
  EmptyPeriod.set_to_date("2023-01-31");
  EXPECT_EQ(get(0, 1, &Response, EmptyPeriod).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
}

// Ни одно исключение не выходит из обработчика.
TEST(GetBarsExceptions, MappedToStatus) {
  core::InMemoryCalendarSource Calendar;
  core::InMemoryRuoniaSource Ruonia;
  const auto StatusOf = [&](std::function<void()> Thrower) {
    ThrowingBarSource Bars(std::move(Thrower));
    worker::Context Context{Bars, Calendar, Ruonia};
    pb::GetBarsRequest Request;
    *Request.mutable_market() = fixtureMarket();
    Request.set_to_step(1);
    pb::GetBarsResponse Response;
    return worker::handleGetBars(Context, Request, &Response);
  };

  EXPECT_EQ(StatusOf([] {
              throw worker::ClickHouseError("секрет: пароль db");
            }).error_code(),
            grpc::StatusCode::UNAVAILABLE);
  EXPECT_EQ(StatusOf([] {
              throw worker::IntervalUnavailable("нет интервала");
            }).error_code(),
            grpc::StatusCode::NOT_FOUND);
  // Повреждённые данные хранилища (бары не по порядку, неверный ответ
  // snapshots_ready) - не «не найдено».
  const grpc::Status Corrupt = StatusOf(
      [] { throw core::MarketDataError("ClickHouse: бары не упорядочены"); });
  EXPECT_EQ(Corrupt.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(Corrupt.error_message().find("ClickHouse"), std::string::npos);
  EXPECT_EQ(StatusOf([] { throw core::SnapshotNotFound("нет"); }).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(StatusOf([] { throw std::invalid_argument("плохо"); }).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  const grpc::Status Internal = StatusOf(
      [] { throw std::runtime_error("внутренняя деталь /etc/passwd"); });
  EXPECT_EQ(Internal.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(Internal.error_message().find("passwd"), std::string::npos);
  EXPECT_EQ(StatusOf([] { throw 42; }).error_code(),
            grpc::StatusCode::INTERNAL);

  const grpc::Status Unavailable =
      StatusOf([] { throw worker::ClickHouseError("секрет: пароль db"); });
  EXPECT_EQ(Unavailable.error_message().find("секрет"), std::string::npos);
}

// Прогрев: бары истории до from_date идут отрицательными шагами, торговые дни
// <= 0.
TEST_F(GetBars, WarmupBarsHaveNegativeSteps) {
  pb::MarketSpec Market = fixtureMarket();
  Market.set_from_date("2024-01-15");
  Market.set_warmup_bars(3);
  pb::GetBarsResponse Response;
  ASSERT_TRUE(get(-3, 0, &Response, Market).ok());
  EXPECT_EQ(Response.total_steps(), 6);
  EXPECT_EQ(Response.warmup_steps(), 3);
  // 10, 11, 12 января (у B нет бара 12-го) и шаг 0 - 15 января.
  ASSERT_EQ(Response.bars_size(), 7);
  const std::int64_t Steps[] = {-3, -3, -2, -2, -1, 0, 0};
  const std::int32_t Days[] = {-2, -2, -1, -1, 0, 1, 1};
  for (int I = 0; I < Response.bars_size(); ++I) {
    EXPECT_EQ(Response.bars(I).step(), Steps[I]) << I;
    EXPECT_EQ(Response.bars(I).trading_day(), Days[I]) << I;
  }
  EXPECT_EQ(Response.bars(4).close().units(), 106);

  pb::GetBarsResponse TooEarly;
  EXPECT_EQ(get(-4, 0, &TooEarly, Market).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  Market.set_warmup_bars(5001);
  EXPECT_EQ(get(0, 0, &TooEarly, Market).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  Market.set_warmup_bars(-1);
  EXPECT_EQ(get(0, 0, &TooEarly, Market).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(GetBars, NoWarmupByDefault) {
  pb::GetBarsResponse Response;
  ASSERT_TRUE(get(0, 0, &Response).ok());
  EXPECT_EQ(Response.warmup_steps(), 0);
  EXPECT_EQ(get(-1, 0, &Response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}
