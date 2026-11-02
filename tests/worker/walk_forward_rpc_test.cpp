#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "clickhouse/clickhouse_client.hpp"
#include "core/market/bar_source.hpp"
#include "core/market/in_memory_bar_source.hpp"
#include "worker_test_support.hpp"

namespace {

using namespace worker_test;

// Приёмник потока: собирает сообщения; после failAfter записей Write возвращает false (клиент закрыл поток).
class CollectingWriter final : public grpc::ServerWriterInterface<pb::WalkForwardUpdate> {
 public:
  explicit CollectingWriter(std::size_t failAfter = static_cast<std::size_t>(-1)) : failAfter_(failAfter) {}

  void SendInitialMetadata() override {}
  using grpc::ServerWriterInterface<pb::WalkForwardUpdate>::Write;
  bool Write(const pb::WalkForwardUpdate& message, grpc::WriteOptions) override {
    if (updates.size() >= failAfter_) return false;
    updates.push_back(message);
    return true;
  }

  std::vector<pb::WalkForwardUpdate> updates;

 private:
  std::size_t failAfter_;
};

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

// Один параметр с тремя значениями; правила работают по обеим бумагам fixture-1.
constexpr const char* kSource =
    "param n = 2 range 1..3 step 1\n"
    "rule \"b\" when close > sma(close, n) and position == 0 then buy 5 lots because \"выше средней {n}\"\n"
    "rule \"s\" when close < sma(close, n) and position > 0 then sell all because \"ниже средней {n}\"\n";

class WalkForwardRpc : public ::testing::Test {
 protected:
  WalkForwardRpc() {
    request_.set_source(kSource);
    request_.set_fit_days(4);  // fixture-1 - 10 торговых дней: окна по умолчанию (250/63) в неё не влезают
    request_.set_test_days(2);
    request_.set_shift_days(2);
  }

  grpc::Status run(CollectingWriter* writer) {
    worker::Context context{fixture_.bars, fixture_.calendar};
    *request_.mutable_market() = market_;
    return worker::handleRunWalkForward(context, request_, writer);
  }

  core::testing::Fixture fixture_ = core::testing::fixture1();
  pb::MarketSpec market_ = fixtureMarket();
  pb::WalkForwardRequest request_;
};

}  // namespace

TEST_F(WalkForwardRpc, SegmentsDatesAndResult) {
  CollectingWriter writer;
  const grpc::Status status = run(&writer);
  ASSERT_TRUE(status.ok()) << status.error_message();

  // Результат - последнее сообщение, и только одно; до него - прогресс.
  ASSERT_GE(writer.updates.size(), 2u);
  for (std::size_t i = 0; i + 1 < writer.updates.size(); ++i) EXPECT_TRUE(writer.updates[i].has_progress()) << i;
  ASSERT_TRUE(writer.updates.back().has_result());
  const pb::WalkForwardResult& result = writer.updates.back().result();

  // Дни 2024-01-09, 10, 11, 12, 15, 16, 17, 18, 19, 22: подбор 4 дня, проверка 2, сдвиг 2 - три отрезка.
  ASSERT_EQ(result.segments_size(), 3);
  struct Dates { const char *fitFrom, *fitTo, *testFrom, *testTo; };
  const Dates expected[] = {{"2024-01-09", "2024-01-12", "2024-01-15", "2024-01-16"},
                            {"2024-01-11", "2024-01-16", "2024-01-17", "2024-01-18"},
                            {"2024-01-15", "2024-01-18", "2024-01-19", "2024-01-22"}};
  double chained = 1.0;
  for (int i = 0; i < 3; ++i) {
    const pb::WalkForwardSegment& s = result.segments(i);
    EXPECT_EQ(s.index(), i);
    EXPECT_EQ(s.fit_from(), expected[i].fitFrom) << i;
    EXPECT_EQ(s.fit_to(), expected[i].fitTo) << i;
    EXPECT_EQ(s.test_from(), expected[i].testFrom) << i;
    EXPECT_EQ(s.test_to(), expected[i].testTo) << i;

    ASSERT_EQ(s.chosen_size(), 1) << i;
    EXPECT_EQ(s.chosen(0).name(), "n");
    EXPECT_GE(s.chosen(0).value(), 1);
    EXPECT_LE(s.chosen(0).value(), 3);

    // Полный результат проверки по срезу в два дня: шаги с 0.
    const pb::RunResult& test = s.test_result();
    EXPECT_EQ(test.status(), pb::COMPLETED);
    EXPECT_EQ(test.total_steps(), 2);
    EXPECT_EQ(test.report().equity_size(), 2);
    for (const pb::Fill& f : test.fills()) EXPECT_LT(f.step(), 2);
    EXPECT_FALSE(test.has_cost_breakdown());
    EXPECT_NE(test.digest(), 0u);
    chained *= 1.0 + test.report().metrics().total_return();
  }
  EXPECT_NEAR(result.chained_return(), chained - 1.0, 1e-9);
}

TEST_F(WalkForwardRpc, ProgressIsMonotoneAndReachesTotal) {
  CollectingWriter writer;
  ASSERT_TRUE(run(&writer).ok());
  // 3 отрезка · (3 набора · 4 шага подбора + 2 шага проверки) = 42.
  std::int64_t previous = 0;
  std::int64_t last = 0;
  for (const pb::WalkForwardUpdate& u : writer.updates) {
    if (!u.has_progress()) continue;
    EXPECT_EQ(u.progress().total_steps(), 42);
    EXPECT_GT(u.progress().done_steps(), previous);
    previous = u.progress().done_steps();
    last = previous;
  }
  EXPECT_EQ(last, 42);
}

TEST_F(WalkForwardRpc, Deterministic) {
  CollectingWriter first, second;
  ASSERT_TRUE(run(&first).ok());
  ASSERT_TRUE(run(&second).ok());
  ASSERT_EQ(first.updates.size(), second.updates.size());
  EXPECT_EQ(first.updates.back().result().SerializeAsString(), second.updates.back().result().SerializeAsString());
}

TEST_F(WalkForwardRpc, ZeroWindowsUseDefaults) {
  // 250/63/63 по умолчанию: в 10 днях нет ни одного отрезка - отказ, а не тихий пустой результат.
  request_.set_fit_days(0);
  request_.set_test_days(0);
  request_.set_shift_days(0);
  CollectingWriter writer;
  const grpc::Status status = run(&writer);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(writer.updates.empty());
}

TEST_F(WalkForwardRpc, GridOverMaxIsInvalidArgument) {
  request_.set_max_grid(2);  // у параметра 3 значения
  CollectingWriter writer;
  const grpc::Status status = run(&writer);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(writer.updates.empty());  // отказ до первого прогона
}

TEST_F(WalkForwardRpc, BadRequestsAreInvalidArgument) {
  CollectingWriter writer;
  request_.set_source("rule \"x\" when close > then buy 1 lots because \"плохо\"\n");
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  // Ошибка проверки (чтение будущего): код диагностики в сообщении.
  request_.set_source("rule \"x\" when close[-1] > 0 then buy 1 lots because \"будущее\"\n");
  const grpc::Status future = run(&writer);
  EXPECT_EQ(future.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(future.error_message().find("E-LA"), std::string::npos) << future.error_message();

  request_.set_source(kSource);
  request_.set_fit_days(-1);
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  request_.set_fit_days(4);

  market_.clear_instruments();
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(writer.updates.empty());
}

TEST_F(WalkForwardRpc, NoDataIsFailedPrecondition) {
  CollectingWriter writer;
  market_.set_snapshot_id("нет-такого");
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(WalkForwardRpc, ClosedStreamIsCancelled) {
  CollectingWriter writer(1);  // принимает одно сообщение прогресса, дальше - клиент ушёл
  const grpc::Status status = run(&writer);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::CANCELLED);
  ASSERT_EQ(writer.updates.size(), 1u);
  EXPECT_TRUE(writer.updates.front().has_progress());
}

// Ни одно исключение не выходит из обработчика.
TEST(WalkForwardRpcExceptions, MappedToStatus) {
  core::InMemoryCalendarSource calendar;
  const auto statusOf = [&](std::function<void()> thrower) {
    ThrowingBarSource bars(std::move(thrower));
    worker::Context context{bars, calendar};
    pb::WalkForwardRequest request;
    *request.mutable_market() = fixtureMarket();
    request.set_source(kSource);
    request.set_fit_days(4);
    request.set_test_days(2);
    request.set_shift_days(2);
    CollectingWriter writer;
    return worker::handleRunWalkForward(context, request, &writer);
  };

  const grpc::Status unavailable = statusOf([] { throw worker::ClickHouseError("секрет: пароль db"); });
  EXPECT_EQ(unavailable.error_code(), grpc::StatusCode::UNAVAILABLE);
  EXPECT_EQ(unavailable.error_message().find("секрет"), std::string::npos);
  EXPECT_EQ(statusOf([] { throw core::SnapshotNotFound("нет"); }).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(statusOf([] { throw std::invalid_argument("плохо"); }).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  const grpc::Status internal = statusOf([] { throw std::runtime_error("внутренняя деталь /etc/passwd"); });
  EXPECT_EQ(internal.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(internal.error_message().find("passwd"), std::string::npos);
  EXPECT_EQ(statusOf([] { throw 42; }).error_code(), grpc::StatusCode::INTERNAL);
}
