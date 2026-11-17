#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "clickhouse/clickhouse_client.hpp"
#include "core/analysis/walk_forward.hpp"
#include "core/market/bar_source.hpp"
#include "core/market/in_memory_bar_source.hpp"
#include "worker_test_support.hpp"

namespace {

using namespace worker_test;

// Приёмник потока: собирает сообщения; после failAfter записей Write возвращает
// false (клиент закрыл поток).
class CollectingWriter final
    : public grpc::ServerWriterInterface<pb::WalkForwardUpdate> {
public:
  explicit CollectingWriter(
      std::size_t FailAfterWrites = static_cast<std::size_t>(-1))
      : FailAfter(FailAfterWrites) {}

  void SendInitialMetadata() override {}
  using grpc::ServerWriterInterface<pb::WalkForwardUpdate>::Write;
  bool Write(const pb::WalkForwardUpdate &Message,
             grpc::WriteOptions) override {
    if (Updates.size() >= FailAfter)
      return false;
    Updates.push_back(Message);
    return true;
  }

  std::vector<pb::WalkForwardUpdate> Updates;

private:
  std::size_t FailAfter;
};

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

// Один параметр с тремя значениями; правила работают по обеим бумагам
// fixture-1.
constexpr const char *kSource =
    "param n = 2 range 1..3 step 1\n"
    "rule \"b\" when close > sma(close, n) and position == 0 then buy 5 lots "
    "because \"выше средней {n}\"\n"
    "rule \"s\" when close < sma(close, n) and position > 0 then sell all "
    "because \"ниже средней {n}\"\n";

class WalkForwardRpc : public ::testing::Test {
protected:
  WalkForwardRpc() {
    Request.set_source(kSource);
    Request.set_fit_days(4); // fixture-1 - 10 торговых дней: окна по умолчанию
                             // (250/63) в неё не влезают
    Request.set_test_days(2);
    Request.set_shift_days(2);
  }

  grpc::Status run(CollectingWriter *Writer) {
    worker::Context Context{Fixture.Bars, Fixture.Calendar};
    *Request.mutable_market() = Market;
    return worker::handleRunWalkForward(Context, Request, Writer);
  }

  core::testing::Fixture Fixture = core::testing::fixture1();
  pb::MarketSpec Market = fixtureMarket();
  pb::WalkForwardRequest Request;
};

} // namespace

TEST_F(WalkForwardRpc, SegmentsDatesAndResult) {
  CollectingWriter Writer;
  const grpc::Status Status = run(&Writer);
  ASSERT_TRUE(Status.ok()) << Status.error_message();

  // Результат - последнее сообщение, и только одно; до него - прогресс.
  ASSERT_GE(Writer.Updates.size(), 2u);
  for (std::size_t I = 0; I + 1 < Writer.Updates.size(); ++I)
    EXPECT_TRUE(Writer.Updates[I].has_progress()) << I;
  ASSERT_TRUE(Writer.Updates.back().has_result());
  const pb::WalkForwardResult &Result = Writer.Updates.back().result();

  // Дни 2024-01-09, 10, 11, 12, 15, 16, 17, 18, 19, 22: подбор 4 дня, проверка
  // 2, сдвиг 2 - три отрезка.
  ASSERT_EQ(Result.segments_size(), 3);
  struct Dates {
    const char *FitFrom, *FitTo, *TestFrom, *TestTo;
  };
  const Dates Expected[] = {
      {"2024-01-09", "2024-01-12", "2024-01-15", "2024-01-16"},
      {"2024-01-11", "2024-01-16", "2024-01-17", "2024-01-18"},
      {"2024-01-15", "2024-01-18", "2024-01-19", "2024-01-22"}};
  double Chained = 1.0;
  for (int I = 0; I < 3; ++I) {
    const pb::WalkForwardSegment &S = Result.segments(I);
    EXPECT_EQ(S.index(), I);
    EXPECT_EQ(S.fit_from(), Expected[I].FitFrom) << I;
    EXPECT_EQ(S.fit_to(), Expected[I].FitTo) << I;
    EXPECT_EQ(S.test_from(), Expected[I].TestFrom) << I;
    EXPECT_EQ(S.test_to(), Expected[I].TestTo) << I;

    ASSERT_EQ(S.chosen_size(), 1) << I;
    EXPECT_EQ(S.chosen(0).name(), "n");
    EXPECT_GE(S.chosen(0).value(), 1);
    EXPECT_LE(S.chosen(0).value(), 3);

    // Полный результат проверки по срезу в два дня: шаги с 0.
    const pb::RunResult &TestResult = S.test_result();
    EXPECT_EQ(TestResult.status(), pb::COMPLETED);
    EXPECT_EQ(TestResult.total_steps(), 2);
    EXPECT_EQ(TestResult.report().equity_size(), 2);
    for (const pb::Fill &F : TestResult.fills())
      EXPECT_LT(F.step(), 2);
    EXPECT_FALSE(TestResult.has_cost_breakdown());
    EXPECT_NE(TestResult.digest(), 0u);
    Chained *= 1.0 + TestResult.report().metrics().total_return();
  }
  EXPECT_NEAR(Result.chained_return(), Chained - 1.0, 1e-9);
}

TEST_F(WalkForwardRpc, ProgressIsMonotoneAndReachesTotal) {
  CollectingWriter Writer;
  ASSERT_TRUE(run(&Writer).ok());
  // 3 отрезка · (3 набора · 4 шага подбора + 2 шага проверки) = 42.
  std::int64_t Previous = 0;
  std::int64_t Last = 0;
  for (const pb::WalkForwardUpdate &U : Writer.Updates) {
    if (!U.has_progress())
      continue;
    EXPECT_EQ(U.progress().total_steps(), 42);
    EXPECT_GT(U.progress().done_steps(), Previous);
    Previous = U.progress().done_steps();
    Last = Previous;
  }
  EXPECT_EQ(Last, 42);
}

TEST_F(WalkForwardRpc, Deterministic) {
  CollectingWriter First, Second;
  ASSERT_TRUE(run(&First).ok());
  ASSERT_TRUE(run(&Second).ok());
  ASSERT_EQ(First.Updates.size(), Second.Updates.size());
  EXPECT_EQ(First.Updates.back().result().SerializeAsString(),
            Second.Updates.back().result().SerializeAsString());
}

TEST_F(WalkForwardRpc, ZeroWindowsUseDefaults) {
  // 250/63/63 по умолчанию: в 10 днях нет ни одного отрезка - отказ, а не тихий
  // пустой результат.
  Request.set_fit_days(0);
  Request.set_test_days(0);
  Request.set_shift_days(0);
  CollectingWriter Writer;
  const grpc::Status Status = run(&Writer);
  EXPECT_EQ(Status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(Writer.Updates.empty());
}

TEST_F(WalkForwardRpc, GridOverMaxIsInvalidArgument) {
  Request.set_max_grid(2); // у параметра 3 значения
  CollectingWriter Writer;
  const grpc::Status Status = run(&Writer);
  EXPECT_EQ(Status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(Writer.Updates.empty()); // отказ до первого прогона
}

TEST_F(WalkForwardRpc, BadRequestsAreInvalidArgument) {
  CollectingWriter Writer;
  Request.set_source(
      "rule \"x\" when close > then buy 1 lots because \"плохо\"\n");
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  // Ошибка проверки (чтение будущего): код диагностики в сообщении.
  Request.set_source(
      "rule \"x\" when close[-1] > 0 then buy 1 lots because \"будущее\"\n");
  const grpc::Status Future = run(&Writer);
  EXPECT_EQ(Future.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(Future.error_message().find("E-LA"), std::string::npos)
      << Future.error_message();

  Request.set_source(kSource);
  Request.set_fit_days(-1);
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  Request.set_fit_days(4);

  Market.clear_instruments();
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(Writer.Updates.empty());
}

TEST_F(WalkForwardRpc, NoDataIsFailedPrecondition) {
  CollectingWriter Writer;
  Market.set_snapshot_id("нет-такого");
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(WalkForwardRpc, ClosedStreamIsCancelled) {
  CollectingWriter Writer(
      1); // принимает одно сообщение прогресса, дальше - клиент ушёл
  const grpc::Status Status = run(&Writer);
  EXPECT_EQ(Status.error_code(), grpc::StatusCode::CANCELLED);
  ASSERT_EQ(Writer.Updates.size(), 1u);
  EXPECT_TRUE(Writer.Updates.front().has_progress());
}

// Ни одно исключение не выходит из обработчика.
TEST(WalkForwardRpcExceptions, MappedToStatus) {
  core::InMemoryCalendarSource Calendar;
  const auto StatusOf = [&](std::function<void()> Thrower) {
    ThrowingBarSource Bars(std::move(Thrower));
    worker::Context Context{Bars, Calendar};
    pb::WalkForwardRequest Request;
    *Request.mutable_market() = fixtureMarket();
    Request.set_source(kSource);
    Request.set_fit_days(4);
    Request.set_test_days(2);
    Request.set_shift_days(2);
    CollectingWriter Writer;
    return worker::handleRunWalkForward(Context, Request, &Writer);
  };

  const grpc::Status Unavailable =
      StatusOf([] { throw worker::ClickHouseError("секрет: пароль db"); });
  EXPECT_EQ(Unavailable.error_code(), grpc::StatusCode::UNAVAILABLE);
  EXPECT_EQ(Unavailable.error_message().find("секрет"), std::string::npos);
  EXPECT_EQ(StatusOf([] { throw core::SnapshotNotFound("нет"); }).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(StatusOf([] { throw std::invalid_argument("плохо"); }).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  // Все прогоны подбора невалидны - доменный исход для стратегии, а не сбой
  // воркера: без повторов, без деталей.
  const grpc::Status NoValid =
      StatusOf([] { throw core::NoValidFitRun("детали /etc/passwd"); });
  EXPECT_EQ(NoValid.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(NoValid.error_message().find("passwd"), std::string::npos);
  const grpc::Status Internal = StatusOf(
      [] { throw std::runtime_error("внутренняя деталь /etc/passwd"); });
  EXPECT_EQ(Internal.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(Internal.error_message().find("passwd"), std::string::npos);
  EXPECT_EQ(StatusOf([] { throw 42; }).error_code(),
            grpc::StatusCode::INTERNAL);
}
