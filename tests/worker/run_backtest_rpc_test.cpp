#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "clickhouse/clickhouse_client.hpp"
#include "core/market/bar_source.hpp"
#include "core/market/in_memory_bar_source.hpp"
#include "core/money/proto_conv.hpp"
#include "worker_service.hpp"
#include "worker_test_support.hpp"

namespace {

using namespace worker_test;

// Приёмник потока: собирает сообщения; после failAfter записей Write возвращает false (клиент закрыл поток).
class CollectingWriter final : public grpc::ServerWriterInterface<pb::RunBacktestUpdate> {
 public:
  explicit CollectingWriter(std::size_t failAfter = static_cast<std::size_t>(-1)) : failAfter_(failAfter) {}

  void SendInitialMetadata() override {}
  using grpc::ServerWriterInterface<pb::RunBacktestUpdate>::Write;
  bool Write(const pb::RunBacktestUpdate& message, grpc::WriteOptions) override {
    if (updates.size() >= failAfter_) return false;
    updates.push_back(message);
    return true;
  }

  std::vector<pb::RunBacktestUpdate> updates;

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

// Один параметр; правила работают по обеим бумагам fixture-1.
constexpr const char* kSource =
    "param n = 2 range 1..3 step 1\n"
    "rule \"b\" when close > sma(close, n) and position == 0 then buy 5 lots because \"выше средней {n}\"\n"
    "rule \"s\" when close < sma(close, n) and position > 0 then sell all because \"ниже средней {n}\"\n";

class RunBacktestRpc : public ::testing::Test {
 protected:
  RunBacktestRpc() {
    request_.mutable_strategy()->set_source(kSource);
    *request_.mutable_market() = fixtureMarket();
  }

  grpc::Status run(CollectingWriter* writer) {
    worker::Context context{fixture_.bars, fixture_.calendar};
    return worker::handleRunBacktest(context, request_, writer);
  }

  // Результат успешного прогона: последнее сообщение потока.
  pb::RunResult result() {
    CollectingWriter writer;
    const grpc::Status status = run(&writer);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(writer.updates.empty());
    return writer.updates.empty() ? pb::RunResult() : writer.updates.back().result();
  }

  void setStress(std::int64_t startStep, const char* gapPercent, std::int32_t volBars) {
    pb::StressScenario* stress = request_.mutable_stress();
    stress->set_start_step(startStep);
    *stress->mutable_gap_percent() = dec(gapPercent);
    *stress->mutable_vol_multiplier() = dec("2");
    stress->set_vol_bars(volBars);
  }

  core::testing::Fixture fixture_ = core::testing::fixture1();
  pb::RunBacktestRequest request_;
};

}  // namespace

TEST_F(RunBacktestRpc, ProgressThenSingleResult) {
  CollectingWriter writer;
  const grpc::Status status = run(&writer);
  ASSERT_TRUE(status.ok()) << status.error_message();

  // Прогресс приходит до результата; результат - последнее сообщение и только одно.
  ASSERT_GE(writer.updates.size(), 2u);
  std::int64_t previous = 0;
  for (std::size_t i = 0; i + 1 < writer.updates.size(); ++i) {
    ASSERT_TRUE(writer.updates[i].has_progress()) << i;
    EXPECT_EQ(writer.updates[i].progress().total_steps(), 10);
    EXPECT_GT(writer.updates[i].progress().done_steps(), previous);
    previous = writer.updates[i].progress().done_steps();
  }
  EXPECT_EQ(previous, 10);
  ASSERT_TRUE(writer.updates.back().has_result());

  const pb::RunResult& result = writer.updates.back().result();
  EXPECT_EQ(result.status(), pb::COMPLETED);
  EXPECT_EQ(result.total_steps(), 10);
  EXPECT_EQ(result.report().equity_size(), 10);
  EXPECT_FALSE(result.fills().empty());
  EXPECT_FALSE(result.has_cost_breakdown());
  EXPECT_NE(result.digest(), 0u);
}

TEST_F(RunBacktestRpc, SameRequestSameDigest) {
  const pb::RunResult first = result();
  const pb::RunResult second = result();
  EXPECT_EQ(first.digest(), second.digest());
  EXPECT_EQ(first.SerializeAsString(), second.SerializeAsString());
}

TEST_F(RunBacktestRpc, ParamOverrideChangesRun) {
  const pb::RunResult base = result();
  pb::ParamValue* n = request_.mutable_strategy()->add_params();
  n->set_name("n");
  n->set_value(3);
  const pb::RunResult changed = result();
  EXPECT_NE(base.digest(), changed.digest());
}

TEST_F(RunBacktestRpc, StressChangesRunAndZeroStressDoesNot) {
  const pb::RunResult base = result();

  request_.mutable_stress();  // все нули - без стресса
  EXPECT_EQ(result().digest(), base.digest());

  setStress(3, "20", 0);
  const pb::RunResult stressed = result();
  EXPECT_NE(stressed.digest(), base.digest());
  EXPECT_EQ(stressed.total_steps(), 10);

  setStress(3, "0", 3);  // только окно волатильности
  EXPECT_NE(result().digest(), base.digest());
}

// Воспроизведение стрессового прогона (GetBars со стрессом прогона) отдаёт те бары, на которых посчитаны сделки:
// базовая цена каждой сделки лежит внутри свечи своего шага. Без стресса бары исходные.
TEST_F(RunBacktestRpc, GetBarsWithStressMatchesBarsOfRun) {
  setStress(3, "20", 0);
  const pb::RunResult stressed = result();
  ASSERT_FALSE(stressed.fills().empty());

  worker::Context context{fixture_.bars, fixture_.calendar};
  const auto barsOf = [&](bool withStress) {
    pb::GetBarsRequest bars;
    *bars.mutable_market() = request_.market();
    bars.set_from_step(0);
    bars.set_to_step(9);
    if (withStress) *bars.mutable_stress() = request_.stress();
    pb::GetBarsResponse response;
    const grpc::Status status = worker::handleGetBars(context, bars, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return response;
  };
  const pb::GetBarsResponse plain = barsOf(false);
  const pb::GetBarsResponse replay = barsOf(true);
  ASSERT_EQ(plain.bars_size(), replay.bars_size());

  const auto inside = [](const pb::GetBarsResponse& response, const pb::Fill& fill) {
    for (const pb::Bar& bar : response.bars()) {
      if (bar.step() != fill.step() || bar.instrument_key() != fill.instrument_key()) continue;
      const core::Decimal price = core::fromProto(fill.base_price());
      return core::fromProto(bar.low()) <= price && price <= core::fromProto(bar.high());
    }
    return false;
  };
  bool stressedFillOutsidePlain = false;
  for (const pb::Fill& fill : stressed.fills()) {
    EXPECT_TRUE(inside(replay, fill)) << "сделка шага " << fill.step() << " вне свечи воспроизведения";
    if (fill.step() >= 3 && !inside(plain, fill)) stressedFillOutsidePlain = true;
  }
  // Тест не вакуумный: без стресса хотя бы одна сделка после начала стресса лежит вне свечи.
  EXPECT_TRUE(stressedFillOutsidePlain);

  // Шаги до начала стресса не меняются, после - меняются; без стресса ответ тот же, что со «пустым» стрессом.
  EXPECT_EQ(plain.bars(0).SerializeAsString(), replay.bars(0).SerializeAsString());
  EXPECT_NE(plain.bars(plain.bars_size() - 1).SerializeAsString(),
            replay.bars(replay.bars_size() - 1).SerializeAsString());
  request_.clear_stress();
  request_.mutable_stress();
  const pb::GetBarsResponse zero = [&] {
    pb::GetBarsRequest bars;
    *bars.mutable_market() = request_.market();
    bars.set_to_step(9);
    *bars.mutable_stress() = request_.stress();
    pb::GetBarsResponse response;
    EXPECT_TRUE(worker::handleGetBars(context, bars, &response).ok());
    return response;
  }();
  EXPECT_EQ(zero.SerializeAsString(), plain.SerializeAsString());
}

TEST_F(RunBacktestRpc, NegativeStartStepIsMiddleOfPeriod) {
  setStress(-1, "20", 0);
  const pb::RunResult middle = result();  // 10 шагов -> шаг 5
  setStress(5, "20", 0);
  EXPECT_EQ(middle.digest(), result().digest());
  setStress(4, "20", 0);
  EXPECT_NE(middle.digest(), result().digest());
}

TEST_F(RunBacktestRpc, CostBreakdownHasSixSteps) {
  request_.set_with_cost_breakdown(true);
  const pb::RunResult withChain = result();
  ASSERT_TRUE(withChain.has_cost_breakdown());
  const pb::CostBreakdown& chain = withChain.cost_breakdown();
  ASSERT_EQ(chain.steps_size(), 6);
  const pb::CostFactor order[] = {pb::NAIVE, pb::DELAY, pb::FEES, pb::SPREAD, pb::SLIPPAGE, pb::LIQUIDITY};
  for (int i = 0; i < 6; ++i) EXPECT_EQ(chain.steps(i).factor(), order[i]) << i;

  // INV-3: последний шаг цепочки - честный прогон, его результат равен итогу обычного прогона.
  const core::Decimal honest = core::fromProto(withChain.report().final_equity()) - core::Decimal::parse("1000000");
  EXPECT_EQ(core::fromProto(chain.steps(5).net_result()), honest);

  // Цепочка ничего не меняет в самом прогоне.
  request_.set_with_cost_breakdown(false);
  const pb::RunResult plain = result();
  EXPECT_EQ(plain.digest(), withChain.digest());
  EXPECT_EQ(plain.report().SerializeAsString(), withChain.report().SerializeAsString());
}

TEST_F(RunBacktestRpc, CostBreakdownWithStressUsesStressedData) {
  request_.set_with_cost_breakdown(true);
  setStress(3, "30", 0);
  const pb::RunResult stressed = result();
  ASSERT_EQ(stressed.cost_breakdown().steps_size(), 6);
  const core::Decimal honest = core::fromProto(stressed.report().final_equity()) - core::Decimal::parse("1000000");
  EXPECT_EQ(core::fromProto(stressed.cost_breakdown().steps(5).net_result()), honest);
}

TEST_F(RunBacktestRpc, CostBreakdownReportsProgressPerStage) {
  request_.set_with_cost_breakdown(true);
  CollectingWriter writer;
  ASSERT_TRUE(run(&writer).ok());
  // Шкала из семи частей по 10 шагов: честный прогон и шесть этапов цепочки.
  std::int64_t previous = 0;
  int messages = 0;
  for (std::size_t i = 0; i + 1 < writer.updates.size(); ++i) {
    ASSERT_TRUE(writer.updates[i].has_progress()) << i;
    EXPECT_EQ(writer.updates[i].progress().total_steps(), 70);
    EXPECT_GT(writer.updates[i].progress().done_steps(), previous);
    previous = writer.updates[i].progress().done_steps();
    ++messages;
  }
  EXPECT_EQ(previous, 70);
  EXPECT_EQ(messages, 7);
  EXPECT_TRUE(writer.updates.back().has_result());
}

TEST_F(RunBacktestRpc, ClosedStreamBetweenCostStagesIsCancelled) {
  request_.set_with_cost_breakdown(true);
  CollectingWriter writer(2);  // прогресс честного прогона и первого этапа, на втором этапе клиент ушёл
  const grpc::Status status = run(&writer);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::CANCELLED);
  ASSERT_EQ(writer.updates.size(), 2u);
  EXPECT_FALSE(writer.updates.back().has_result());
}

TEST_F(RunBacktestRpc, StressStartAfterEndIsInvalidArgument) {
  CollectingWriter writer;
  setStress(10, "20", 0);  // шагов 10: последний - 9
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  setStress(11, "20", 0);
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(writer.updates.empty());
  setStress(9, "20", 0);  // последний шаг - ещё допустимо
  EXPECT_TRUE(run(&writer).ok());
}

TEST_F(RunBacktestRpc, CostBreakdownOfNaiveRunIsInvalidArgument) {
  request_.set_with_cost_breakdown(true);
  request_.mutable_market()->set_naive(true);
  CollectingWriter writer;
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(writer.updates.empty());
}

TEST_F(RunBacktestRpc, FutureReadIsInvalidArgumentWithCode) {
  request_.mutable_strategy()->set_source("rule \"x\" when close[-1] > 0 then buy 1 lots because \"будущее\"\n");
  CollectingWriter writer;
  const grpc::Status status = run(&writer);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(status.error_message().find("E-LA01"), std::string::npos) << status.error_message();
  EXPECT_TRUE(writer.updates.empty());
}

TEST_F(RunBacktestRpc, BadRequestsAreInvalidArgument) {
  CollectingWriter writer;

  // Ошибка разбора.
  request_.mutable_strategy()->set_source("rule \"x\" when close > then buy 1 lots because \"плохо\"\n");
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  request_.mutable_strategy()->set_source(kSource);

  // Значение параметра вне диапазона, неизвестный параметр, повтор параметра.
  pb::ParamValue* n = request_.mutable_strategy()->add_params();
  n->set_name("n");
  n->set_value(10);
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  n->set_value(2);
  n->set_name("нет-такого");
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  n->set_name("n");
  pb::ParamValue* twice = request_.mutable_strategy()->add_params();
  twice->set_name("n");
  twice->set_value(3);
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  request_.mutable_strategy()->clear_params();

  // Стресс вне диапазона: гэп >= 100 %, отрицательные окно и множитель.
  setStress(3, "100", 0);
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  setStress(3, "0", -2);
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  setStress(3, "0", 0);
  *request_.mutable_stress()->mutable_vol_multiplier() = dec("-1");
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  request_.clear_stress();

  request_.mutable_market()->clear_instruments();
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(writer.updates.empty());  // отказ до первого шага прогона
}

TEST_F(RunBacktestRpc, NoDataIsFailedPrecondition) {
  request_.mutable_market()->set_snapshot_id("нет-такого");
  CollectingWriter writer;
  EXPECT_EQ(run(&writer).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(RunBacktestRpc, ClosedStreamIsCancelled) {
  CollectingWriter writer(1);  // принимает одно сообщение прогресса, дальше - клиент ушёл
  const grpc::Status status = run(&writer);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::CANCELLED);
  ASSERT_EQ(writer.updates.size(), 1u);
  EXPECT_TRUE(writer.updates.front().has_progress());
}

// Ни одно исключение не выходит из обработчика.
TEST(RunBacktestRpcExceptions, MappedToStatus) {
  core::InMemoryCalendarSource calendar;
  const auto statusOf = [&](std::function<void()> thrower) {
    ThrowingBarSource bars(std::move(thrower));
    worker::Context context{bars, calendar};
    pb::RunBacktestRequest request;
    *request.mutable_market() = fixtureMarket();
    request.mutable_strategy()->set_source(kSource);
    CollectingWriter writer;
    return worker::handleRunBacktest(context, request, &writer);
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

// Тот же запрос по настоящему gRPC: поток идёт целиком, digest одинаков при повторе (INV-4).
TEST(RunBacktestRpcOverGrpc, StreamAndDeterministicDigest) {
  core::testing::Fixture fixture = core::testing::fixture1();
  worker::WorkerService service(worker::Context{fixture.bars, fixture.calendar});
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  ASSERT_TRUE(server);
  auto stub = pb::SimulationWorker::NewStub(
      grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));

  pb::RunBacktestRequest request;
  *request.mutable_market() = fixtureMarket();
  request.mutable_strategy()->set_source(kSource);
  request.set_with_cost_breakdown(true);

  const auto call = [&](std::vector<pb::RunBacktestUpdate>* updates) {
    grpc::ClientContext context;
    auto reader = stub->RunBacktest(&context, request);
    pb::RunBacktestUpdate update;
    while (reader->Read(&update)) updates->push_back(update);
    return reader->Finish();
  };
  std::vector<pb::RunBacktestUpdate> first, second;
  ASSERT_TRUE(call(&first).ok());
  ASSERT_TRUE(call(&second).ok());
  ASSERT_GE(first.size(), 2u);
  EXPECT_TRUE(first.front().has_progress());
  ASSERT_TRUE(first.back().has_result());
  ASSERT_TRUE(second.back().has_result());
  EXPECT_EQ(first.back().result().cost_breakdown().steps_size(), 6);
  EXPECT_EQ(first.back().result().digest(), second.back().result().digest());
  EXPECT_EQ(first.back().result().SerializeAsString(), second.back().result().SerializeAsString());

  // Пустой запрос - отказ, а не заглушка.
  grpc::ClientContext context;
  auto reader = stub->RunBacktest(&context, pb::RunBacktestRequest());
  pb::RunBacktestUpdate update;
  EXPECT_FALSE(reader->Read(&update));
  EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  server->Shutdown();
}
