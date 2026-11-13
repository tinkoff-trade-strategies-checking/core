#include <gtest/gtest.h>

#include <functional>
#include <stdexcept>
#include <string>

#include "core/market/bar_source.hpp"
#include "clickhouse/clickhouse_client.hpp"
#include "convert/market.hpp"
#include "worker_test_support.hpp"

namespace {

using namespace worker_test;

// Источник, который бросает заданное исключение при любом чтении.
class ThrowingBarSource final : public core::BarSource {
 public:
  explicit ThrowingBarSource(std::function<void()> thrower) : thrower_(std::move(thrower)) {}
  std::vector<core::Bar> load(const core::BarRequest&) override {
    thrower_();
    return {};
  }
  core::Security security(const std::string&, const std::string&) override {
    thrower_();
    return {};
  }

 private:
  std::function<void()> thrower_;
};

class GetBars : public ::testing::Test {
 protected:
  grpc::Status get(std::int64_t from, std::int64_t to, pb::GetBarsResponse* response,
                   pb::MarketSpec market = fixtureMarket()) {
    worker::Context context{fixture_.bars, fixture_.calendar};
    pb::GetBarsRequest request;
    *request.mutable_market() = std::move(market);
    request.set_from_step(from);
    request.set_to_step(to);
    return worker::handleGetBars(context, request, response);
  }

  core::testing::Fixture fixture_ = core::testing::fixture1();
};

}  // namespace

TEST_F(GetBars, Range) {
  pb::GetBarsResponse response;
  ASSERT_TRUE(get(2, 4, &response).ok());
  EXPECT_EQ(response.total_steps(), 10);
  // Шаги 2, 3, 4: у A бары на всех, у B нет бара на шаге 3. Порядок: шаг, затем бумага.
  ASSERT_EQ(response.bars_size(), 5);
  const char* const keys[] = {"A", "B", "A", "A", "B"};
  const std::int64_t steps[] = {2, 2, 3, 4, 4};
  for (int i = 0; i < response.bars_size(); ++i) {
    EXPECT_EQ(response.bars(i).step(), steps[i]) << i;
    EXPECT_EQ(response.bars(i).instrument_key(), keys[i]) << i;
  }
  const pb::Bar& bar = response.bars(0);
  EXPECT_EQ(bar.close().units(), 103);
  EXPECT_EQ(bar.close().nano(), 0);
  EXPECT_EQ(bar.open().units(), 101);
  EXPECT_EQ(bar.volume_lots(), 10000);  // 100000 штук / лот 10
  EXPECT_EQ(bar.trading_day(), 3);
  EXPECT_FALSE(bar.after_halt());
  EXPECT_EQ(response.bars(1).close().units(), 0);
  EXPECT_EQ(response.bars(1).close().nano(), 20000000);  // 0.02
  EXPECT_EQ(response.bars(1).volume_lots(), 100000);     // 1e9 штук / лот 10000
}

TEST_F(GetBars, SingleStepAndWholeRange) {
  pb::GetBarsResponse one;
  ASSERT_TRUE(get(9, 9, &one).ok());
  ASSERT_EQ(one.bars_size(), 2);
  EXPECT_EQ(one.bars(0).step(), 9);

  pb::GetBarsResponse all;
  ASSERT_TRUE(get(0, 9, &all).ok());
  EXPECT_EQ(all.bars_size(), 19);  // 10 баров A и 9 баров B
}

TEST_F(GetBars, BarAfterGapIsMarked) {
  // У B нет бара 12 января: следующий бар B (шаг 4) идёт после разрыва торгов.
  pb::GetBarsResponse response;
  ASSERT_TRUE(get(4, 4, &response).ok());
  ASSERT_EQ(response.bars_size(), 2);
  EXPECT_EQ(response.bars(1).instrument_key(), "B");
  EXPECT_TRUE(response.bars(1).after_halt());
}

TEST_F(GetBars, BadRangeIsInvalidArgument) {
  pb::GetBarsResponse response;
  EXPECT_EQ(get(0, 10, &response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(get(-1, 3, &response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(get(5, 4, &response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(response.bars_size(), 0);
}

TEST_F(GetBars, BadSpecIsInvalidArgument) {
  pb::GetBarsResponse response;
  pb::MarketSpec badDate = fixtureMarket();
  badDate.set_from_date("09.01.2024");
  EXPECT_EQ(get(0, 1, &response, badDate).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  pb::MarketSpec noTimeframe = fixtureMarket();
  noTimeframe.set_timeframe(pb::TIMEFRAME_UNSPECIFIED);
  EXPECT_EQ(get(0, 1, &response, noTimeframe).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  pb::MarketSpec noInstruments = fixtureMarket();
  noInstruments.clear_instruments();
  EXPECT_EQ(get(0, 1, &response, noInstruments).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(GetBars, NoDataIsFailedPrecondition) {
  pb::GetBarsResponse response;
  pb::MarketSpec unknownSnapshot = fixtureMarket();
  unknownSnapshot.set_snapshot_id("нет-такого");
  EXPECT_EQ(get(0, 1, &response, unknownSnapshot).error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  pb::MarketSpec unknownTicker = fixtureMarket();
  unknownTicker.mutable_instruments(0)->set_ticker("NOPE");
  EXPECT_EQ(get(0, 1, &response, unknownTicker).error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  pb::MarketSpec emptyPeriod = fixtureMarket();
  emptyPeriod.set_from_date("2023-01-01");
  emptyPeriod.set_to_date("2023-01-31");
  EXPECT_EQ(get(0, 1, &response, emptyPeriod).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// Ни одно исключение не выходит из обработчика.
TEST(GetBarsExceptions, MappedToStatus) {
  core::InMemoryCalendarSource calendar;
  const auto statusOf = [&](std::function<void()> thrower) {
    ThrowingBarSource bars(std::move(thrower));
    worker::Context context{bars, calendar};
    pb::GetBarsRequest request;
    *request.mutable_market() = fixtureMarket();
    request.set_to_step(1);
    pb::GetBarsResponse response;
    return worker::handleGetBars(context, request, &response);
  };

  EXPECT_EQ(statusOf([] { throw worker::ClickHouseError("секрет: пароль db"); }).error_code(),
            grpc::StatusCode::UNAVAILABLE);
  EXPECT_EQ(statusOf([] { throw worker::IntervalUnavailable("нет интервала"); }).error_code(),
            grpc::StatusCode::NOT_FOUND);
  // Повреждённые данные хранилища (бары не по порядку, неверный ответ snapshots_ready) - не «не найдено».
  const grpc::Status corrupt = statusOf([] { throw core::MarketDataError("ClickHouse: бары не упорядочены"); });
  EXPECT_EQ(corrupt.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(corrupt.error_message().find("ClickHouse"), std::string::npos);
  EXPECT_EQ(statusOf([] { throw core::SnapshotNotFound("нет"); }).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(statusOf([] { throw std::invalid_argument("плохо"); }).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  const grpc::Status internal = statusOf([] { throw std::runtime_error("внутренняя деталь /etc/passwd"); });
  EXPECT_EQ(internal.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(internal.error_message().find("passwd"), std::string::npos);
  EXPECT_EQ(statusOf([] { throw 42; }).error_code(), grpc::StatusCode::INTERNAL);

  const grpc::Status unavailable = statusOf([] { throw worker::ClickHouseError("секрет: пароль db"); });
  EXPECT_EQ(unavailable.error_message().find("секрет"), std::string::npos);
}

// Прогрев: бары истории до from_date идут отрицательными шагами, торговые дни <= 0.
TEST_F(GetBars, WarmupBarsHaveNegativeSteps) {
  pb::MarketSpec market = fixtureMarket();
  market.set_from_date("2024-01-15");
  market.set_warmup_bars(3);
  pb::GetBarsResponse response;
  ASSERT_TRUE(get(-3, 0, &response, market).ok());
  EXPECT_EQ(response.total_steps(), 6);
  EXPECT_EQ(response.warmup_steps(), 3);
  // 10, 11, 12 января (у B нет бара 12-го) и шаг 0 - 15 января.
  ASSERT_EQ(response.bars_size(), 7);
  const std::int64_t steps[] = {-3, -3, -2, -2, -1, 0, 0};
  const std::int32_t days[] = {-2, -2, -1, -1, 0, 1, 1};
  for (int i = 0; i < response.bars_size(); ++i) {
    EXPECT_EQ(response.bars(i).step(), steps[i]) << i;
    EXPECT_EQ(response.bars(i).trading_day(), days[i]) << i;
  }
  EXPECT_EQ(response.bars(4).close().units(), 106);

  pb::GetBarsResponse tooEarly;
  EXPECT_EQ(get(-4, 0, &tooEarly, market).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  market.set_warmup_bars(5001);
  EXPECT_EQ(get(0, 0, &tooEarly, market).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  market.set_warmup_bars(-1);
  EXPECT_EQ(get(0, 0, &tooEarly, market).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(GetBars, NoWarmupByDefault) {
  pb::GetBarsResponse response;
  ASSERT_TRUE(get(0, 0, &response).ok());
  EXPECT_EQ(response.warmup_steps(), 0);
  EXPECT_EQ(get(-1, 0, &response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
