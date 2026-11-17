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

// Приёмник потока: собирает сообщения; после failAfter записей Write возвращает
// false (клиент закрыл поток).
class CollectingWriter final
    : public grpc::ServerWriterInterface<pb::RunBacktestUpdate> {
public:
  explicit CollectingWriter(
      std::size_t FailAfterWrites = static_cast<std::size_t>(-1))
      : FailAfter(FailAfterWrites) {}

  void SendInitialMetadata() override {}
  using grpc::ServerWriterInterface<pb::RunBacktestUpdate>::Write;
  bool Write(const pb::RunBacktestUpdate &Message,
             grpc::WriteOptions) override {
    if (Updates.size() >= FailAfter)
      return false;
    Updates.push_back(Message);
    return true;
  }

  std::vector<pb::RunBacktestUpdate> Updates;

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

// Один параметр; правила работают по обеим бумагам fixture-1.
constexpr const char *kSource =
    "param n = 2 range 1..3 step 1\n"
    "rule \"b\" when close > sma(close, n) and position == 0 then buy 5 lots "
    "because \"выше средней {n}\"\n"
    "rule \"s\" when close < sma(close, n) and position > 0 then sell all "
    "because \"ниже средней {n}\"\n";

class RunBacktestRpc : public ::testing::Test {
protected:
  RunBacktestRpc() {
    Request.mutable_strategy()->set_source(kSource);
    *Request.mutable_market() = fixtureMarket();
  }

  grpc::Status run(CollectingWriter *Writer) {
    worker::Context Context{Fixture.Bars, Fixture.Calendar};
    return worker::handleRunBacktest(Context, Request, Writer);
  }

  // Результат успешного прогона: последнее сообщение потока.
  pb::RunResult result() {
    CollectingWriter Writer;
    const grpc::Status Status = run(&Writer);
    EXPECT_TRUE(Status.ok()) << Status.error_message();
    EXPECT_FALSE(Writer.Updates.empty());
    return Writer.Updates.empty() ? pb::RunResult()
                                  : Writer.Updates.back().result();
  }

  void setStress(std::int64_t StartStep, const char *GapPercent,
                 std::int32_t VolBars) {
    pb::StressScenario *Stress = Request.mutable_stress();
    Stress->set_start_step(StartStep);
    *Stress->mutable_gap_percent() = dec(GapPercent);
    *Stress->mutable_vol_multiplier() = dec("2");
    Stress->set_vol_bars(VolBars);
  }

  core::testing::Fixture Fixture = core::testing::fixture1();
  pb::RunBacktestRequest Request;
};

} // namespace

TEST_F(RunBacktestRpc, ProgressThenSingleResult) {
  CollectingWriter Writer;
  const grpc::Status Status = run(&Writer);
  ASSERT_TRUE(Status.ok()) << Status.error_message();

  // Прогресс приходит до результата; результат - последнее сообщение и только
  // одно.
  ASSERT_GE(Writer.Updates.size(), 2u);
  std::int64_t Previous = 0;
  for (std::size_t I = 0; I + 1 < Writer.Updates.size(); ++I) {
    ASSERT_TRUE(Writer.Updates[I].has_progress()) << I;
    EXPECT_EQ(Writer.Updates[I].progress().total_steps(), 10);
    EXPECT_GT(Writer.Updates[I].progress().done_steps(), Previous);
    Previous = Writer.Updates[I].progress().done_steps();
  }
  EXPECT_EQ(Previous, 10);
  ASSERT_TRUE(Writer.Updates.back().has_result());

  const pb::RunResult &Result = Writer.Updates.back().result();
  EXPECT_EQ(Result.status(), pb::COMPLETED);
  EXPECT_EQ(Result.total_steps(), 10);
  EXPECT_EQ(Result.report().equity_size(), 10);
  EXPECT_FALSE(Result.fills().empty());
  EXPECT_FALSE(Result.has_cost_breakdown());
  EXPECT_NE(Result.digest(), 0u);
}

TEST_F(RunBacktestRpc, SameRequestSameDigest) {
  const pb::RunResult First = result();
  const pb::RunResult Second = result();
  EXPECT_EQ(First.digest(), Second.digest());
  EXPECT_EQ(First.SerializeAsString(), Second.SerializeAsString());
}

TEST_F(RunBacktestRpc, ParamOverrideChangesRun) {
  const pb::RunResult Base = result();
  pb::ParamValue *N = Request.mutable_strategy()->add_params();
  N->set_name("n");
  N->set_value(3);
  const pb::RunResult Changed = result();
  EXPECT_NE(Base.digest(), Changed.digest());
}

TEST_F(RunBacktestRpc, StressChangesRunAndZeroStressDoesNot) {
  const pb::RunResult Base = result();

  Request.mutable_stress(); // все нули - без стресса
  EXPECT_EQ(result().digest(), Base.digest());

  setStress(3, "20", 0);
  const pb::RunResult Stressed = result();
  EXPECT_NE(Stressed.digest(), Base.digest());
  EXPECT_EQ(Stressed.total_steps(), 10);

  setStress(3, "0", 3); // только окно волатильности
  EXPECT_NE(result().digest(), Base.digest());
}

// Воспроизведение стрессового прогона (GetBars со стрессом прогона) отдаёт те
// бары, на которых посчитаны сделки: базовая цена каждой сделки лежит внутри
// свечи своего шага. Без стресса бары исходные.
TEST_F(RunBacktestRpc, GetBarsWithStressMatchesBarsOfRun) {
  setStress(3, "20", 0);
  const pb::RunResult Stressed = result();
  ASSERT_FALSE(Stressed.fills().empty());

  worker::Context Context{Fixture.Bars, Fixture.Calendar};
  const auto BarsOf = [&](bool WithStress) {
    pb::GetBarsRequest Bars;
    *Bars.mutable_market() = Request.market();
    Bars.set_from_step(0);
    Bars.set_to_step(9);
    if (WithStress)
      *Bars.mutable_stress() = Request.stress();
    pb::GetBarsResponse Response;
    const grpc::Status Status = worker::handleGetBars(Context, Bars, &Response);
    EXPECT_TRUE(Status.ok()) << Status.error_message();
    return Response;
  };
  const pb::GetBarsResponse Plain = BarsOf(false);
  const pb::GetBarsResponse Replay = BarsOf(true);
  ASSERT_EQ(Plain.bars_size(), Replay.bars_size());

  const auto Inside = [](const pb::GetBarsResponse &Response,
                         const pb::Fill &Fill) {
    for (const pb::Bar &Bar : Response.bars()) {
      if (Bar.step() != Fill.step() ||
          Bar.instrument_key() != Fill.instrument_key())
        continue;
      const core::Decimal Price = core::fromProto(Fill.base_price());
      return core::fromProto(Bar.low()) <= Price &&
             Price <= core::fromProto(Bar.high());
    }
    return false;
  };
  bool StressedFillOutsidePlain = false;
  for (const pb::Fill &Fill : Stressed.fills()) {
    EXPECT_TRUE(Inside(Replay, Fill))
        << "сделка шага " << Fill.step() << " вне свечи воспроизведения";
    if (Fill.step() >= 3 && !Inside(Plain, Fill))
      StressedFillOutsidePlain = true;
  }
  // Тест не вакуумный: без стресса хотя бы одна сделка после начала стресса
  // лежит вне свечи.
  EXPECT_TRUE(StressedFillOutsidePlain);

  // Шаги до начала стресса не меняются, после - меняются; без стресса ответ тот
  // же, что со «пустым» стрессом.
  EXPECT_EQ(Plain.bars(0).SerializeAsString(),
            Replay.bars(0).SerializeAsString());
  EXPECT_NE(Plain.bars(Plain.bars_size() - 1).SerializeAsString(),
            Replay.bars(Replay.bars_size() - 1).SerializeAsString());
  Request.clear_stress();
  Request.mutable_stress();
  const pb::GetBarsResponse Zero = [&] {
    pb::GetBarsRequest Bars;
    *Bars.mutable_market() = Request.market();
    Bars.set_to_step(9);
    *Bars.mutable_stress() = Request.stress();
    pb::GetBarsResponse Response;
    EXPECT_TRUE(worker::handleGetBars(Context, Bars, &Response).ok());
    return Response;
  }();
  EXPECT_EQ(Zero.SerializeAsString(), Plain.SerializeAsString());
}

TEST_F(RunBacktestRpc, NegativeStartStepIsMiddleOfPeriod) {
  setStress(-1, "20", 0);
  const pb::RunResult Middle = result(); // 10 шагов -> шаг 5
  setStress(5, "20", 0);
  EXPECT_EQ(Middle.digest(), result().digest());
  setStress(4, "20", 0);
  EXPECT_NE(Middle.digest(), result().digest());
}

TEST_F(RunBacktestRpc, CostBreakdownHasSixSteps) {
  Request.set_with_cost_breakdown(true);
  const pb::RunResult WithChain = result();
  ASSERT_TRUE(WithChain.has_cost_breakdown());
  const pb::CostBreakdown &Chain = WithChain.cost_breakdown();
  ASSERT_EQ(Chain.steps_size(), 6);
  const pb::CostFactor Order[] = {pb::NAIVE,  pb::DELAY,    pb::FEES,
                                  pb::SPREAD, pb::SLIPPAGE, pb::LIQUIDITY};
  for (int I = 0; I < 6; ++I)
    EXPECT_EQ(Chain.steps(I).factor(), Order[I]) << I;

  // INV-3: последний шаг цепочки - честный прогон, его результат равен итогу
  // обычного прогона.
  const core::Decimal Honest =
      core::fromProto(WithChain.report().final_equity()) -
      core::Decimal::parse("1000000");
  EXPECT_EQ(core::fromProto(Chain.steps(5).net_result()), Honest);

  // Цепочка ничего не меняет в самом прогоне.
  Request.set_with_cost_breakdown(false);
  const pb::RunResult Plain = result();
  EXPECT_EQ(Plain.digest(), WithChain.digest());
  EXPECT_EQ(Plain.report().SerializeAsString(),
            WithChain.report().SerializeAsString());
}

TEST_F(RunBacktestRpc, CostBreakdownWithStressUsesStressedData) {
  Request.set_with_cost_breakdown(true);
  setStress(3, "30", 0);
  const pb::RunResult Stressed = result();
  ASSERT_EQ(Stressed.cost_breakdown().steps_size(), 6);
  const core::Decimal Honest =
      core::fromProto(Stressed.report().final_equity()) -
      core::Decimal::parse("1000000");
  EXPECT_EQ(core::fromProto(Stressed.cost_breakdown().steps(5).net_result()),
            Honest);
}

TEST_F(RunBacktestRpc, CostBreakdownReportsProgressPerStage) {
  Request.set_with_cost_breakdown(true);
  CollectingWriter Writer;
  ASSERT_TRUE(run(&Writer).ok());
  // Шкала из семи частей по 10 шагов: честный прогон и шесть этапов цепочки.
  std::int64_t Previous = 0;
  int Messages = 0;
  for (std::size_t I = 0; I + 1 < Writer.Updates.size(); ++I) {
    ASSERT_TRUE(Writer.Updates[I].has_progress()) << I;
    EXPECT_EQ(Writer.Updates[I].progress().total_steps(), 70);
    EXPECT_GT(Writer.Updates[I].progress().done_steps(), Previous);
    Previous = Writer.Updates[I].progress().done_steps();
    ++Messages;
  }
  EXPECT_EQ(Previous, 70);
  EXPECT_EQ(Messages, 7);
  EXPECT_TRUE(Writer.Updates.back().has_result());
}

TEST_F(RunBacktestRpc, ClosedStreamBetweenCostStagesIsCancelled) {
  Request.set_with_cost_breakdown(true);
  CollectingWriter Writer(2); // прогресс честного прогона и первого этапа, на
                              // втором этапе клиент ушёл
  const grpc::Status Status = run(&Writer);
  EXPECT_EQ(Status.error_code(), grpc::StatusCode::CANCELLED);
  ASSERT_EQ(Writer.Updates.size(), 2u);
  EXPECT_FALSE(Writer.Updates.back().has_result());
}

TEST_F(RunBacktestRpc, StressStartAfterEndIsInvalidArgument) {
  CollectingWriter Writer;
  setStress(10, "20", 0); // шагов 10: последний - 9
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  setStress(11, "20", 0);
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(Writer.Updates.empty());
  setStress(9, "20", 0); // последний шаг - ещё допустимо
  EXPECT_TRUE(run(&Writer).ok());
}

TEST_F(RunBacktestRpc, CostBreakdownOfNaiveRunIsInvalidArgument) {
  Request.set_with_cost_breakdown(true);
  Request.mutable_market()->set_naive(true);
  CollectingWriter Writer;
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(Writer.Updates.empty());
}

TEST_F(RunBacktestRpc, FutureReadIsInvalidArgumentWithCode) {
  Request.mutable_strategy()->set_source(
      "rule \"x\" when close[-1] > 0 then buy 1 lots because \"будущее\"\n");
  CollectingWriter Writer;
  const grpc::Status Status = run(&Writer);
  EXPECT_EQ(Status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(Status.error_message().find("E-LA01"), std::string::npos)
      << Status.error_message();
  EXPECT_TRUE(Writer.Updates.empty());
}

TEST_F(RunBacktestRpc, BadRequestsAreInvalidArgument) {
  CollectingWriter Writer;

  // Ошибка разбора.
  Request.mutable_strategy()->set_source(
      "rule \"x\" when close > then buy 1 lots because \"плохо\"\n");
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  Request.mutable_strategy()->set_source(kSource);

  // Значение параметра вне диапазона, неизвестный параметр, повтор параметра.
  pb::ParamValue *N = Request.mutable_strategy()->add_params();
  N->set_name("n");
  N->set_value(10);
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  N->set_value(2);
  N->set_name("нет-такого");
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  N->set_name("n");
  pb::ParamValue *Twice = Request.mutable_strategy()->add_params();
  Twice->set_name("n");
  Twice->set_value(3);
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  Request.mutable_strategy()->clear_params();

  // Стресс вне диапазона: гэп >= 100 %, отрицательные окно и множитель.
  setStress(3, "100", 0);
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  setStress(3, "0", -2);
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  setStress(3, "0", 0);
  *Request.mutable_stress()->mutable_vol_multiplier() = dec("-1");
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  Request.clear_stress();

  Request.mutable_market()->clear_instruments();
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(Writer.Updates.empty()); // отказ до первого шага прогона
}

TEST_F(RunBacktestRpc, NoDataIsFailedPrecondition) {
  Request.mutable_market()->set_snapshot_id("нет-такого");
  CollectingWriter Writer;
  EXPECT_EQ(run(&Writer).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(RunBacktestRpc, ClosedStreamIsCancelled) {
  CollectingWriter Writer(
      1); // принимает одно сообщение прогресса, дальше - клиент ушёл
  const grpc::Status Status = run(&Writer);
  EXPECT_EQ(Status.error_code(), grpc::StatusCode::CANCELLED);
  ASSERT_EQ(Writer.Updates.size(), 1u);
  EXPECT_TRUE(Writer.Updates.front().has_progress());
}

// Ни одно исключение не выходит из обработчика.
TEST(RunBacktestRpcExceptions, MappedToStatus) {
  core::InMemoryCalendarSource Calendar;
  const auto StatusOf = [&](std::function<void()> Thrower) {
    ThrowingBarSource Bars(std::move(Thrower));
    worker::Context Context{Bars, Calendar};
    pb::RunBacktestRequest Request;
    *Request.mutable_market() = fixtureMarket();
    Request.mutable_strategy()->set_source(kSource);
    CollectingWriter Writer;
    return worker::handleRunBacktest(Context, Request, &Writer);
  };

  const grpc::Status Unavailable =
      StatusOf([] { throw worker::ClickHouseError("секрет: пароль db"); });
  EXPECT_EQ(Unavailable.error_code(), grpc::StatusCode::UNAVAILABLE);
  EXPECT_EQ(Unavailable.error_message().find("секрет"), std::string::npos);
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
}

// Тот же запрос по настоящему gRPC: поток идёт целиком, digest одинаков при
// повторе (INV-4).
TEST(RunBacktestRpcOverGrpc, StreamAndDeterministicDigest) {
  core::testing::Fixture Fixture = core::testing::fixture1();
  worker::WorkerService Service(
      worker::Context{Fixture.Bars, Fixture.Calendar});
  int Port = 0;
  grpc::ServerBuilder Builder;
  Builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &Port);
  Builder.RegisterService(&Service);
  std::unique_ptr<grpc::Server> Server = Builder.BuildAndStart();
  ASSERT_TRUE(Server);
  auto Stub = pb::SimulationWorker::NewStub(grpc::CreateChannel(
      "127.0.0.1:" + std::to_string(Port), grpc::InsecureChannelCredentials()));

  pb::RunBacktestRequest Request;
  *Request.mutable_market() = fixtureMarket();
  Request.mutable_strategy()->set_source(kSource);
  Request.set_with_cost_breakdown(true);

  const auto Call = [&](std::vector<pb::RunBacktestUpdate> *Updates) {
    grpc::ClientContext Context;
    auto Reader = Stub->RunBacktest(&Context, Request);
    pb::RunBacktestUpdate Update;
    while (Reader->Read(&Update))
      Updates->push_back(Update);
    return Reader->Finish();
  };
  std::vector<pb::RunBacktestUpdate> First, Second;
  ASSERT_TRUE(Call(&First).ok());
  ASSERT_TRUE(Call(&Second).ok());
  ASSERT_GE(First.size(), 2u);
  EXPECT_TRUE(First.front().has_progress());
  ASSERT_TRUE(First.back().has_result());
  ASSERT_TRUE(Second.back().has_result());
  EXPECT_EQ(First.back().result().cost_breakdown().steps_size(), 6);
  EXPECT_EQ(First.back().result().digest(), Second.back().result().digest());
  EXPECT_EQ(First.back().result().SerializeAsString(),
            Second.back().result().SerializeAsString());

  // Пустой запрос - отказ, а не заглушка.
  grpc::ClientContext Context;
  auto Reader = Stub->RunBacktest(&Context, pb::RunBacktestRequest());
  pb::RunBacktestUpdate Update;
  EXPECT_FALSE(Reader->Read(&Update));
  EXPECT_EQ(Reader->Finish().error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  Server->Shutdown();
}

// Прогрев: стратегия видит историю до from_date с шага 0, торговля и кривая
// стоимости - только в периоде.
TEST_F(RunBacktestRpc, WarmupLetsIndicatorsStartAtStepZero) {
  Request.mutable_strategy()->set_source(
      "rule \"b\" when close > close[2] and position == 0 then buy 1 lots "
      "because \"рост за два бара\"\n");
  Request.mutable_market()->set_from_date("2024-01-11");
  const pb::RunResult Plain = result();
  ASSERT_FALSE(Plain.fills().empty());
  EXPECT_EQ(Plain.fills(0).step(), 3);

  Request.mutable_market()->set_warmup_bars(2);
  const pb::RunResult Warm = result();
  ASSERT_FALSE(Warm.fills().empty());
  EXPECT_EQ(Warm.fills(0).step(), 1);
  EXPECT_EQ(Warm.total_steps(), 8);
  EXPECT_EQ(Warm.report().equity_size(), 8);
  EXPECT_EQ(Warm.report().equity(0).step(), 0);
  EXPECT_EQ(Warm.report().metrics().stop_take_same_bar(), 0);
}

// Журнал прогона: ReplaySession по нему (тот же MarketSpec и стресс) повторяет
// прогон один в один (INV-3) - воспроизведение алгоритма на курсоре.
static void expectJournalReplaysRun(core::testing::Fixture &Fixture,
                                    const pb::RunBacktestRequest &RunRequest,
                                    const pb::RunResult &Run) {
  ASSERT_FALSE(Run.journal().empty());
  for (const pb::JournalEntry &E : Run.journal())
    EXPECT_FALSE(E.rule_name().empty());

  worker::Context Context{Fixture.Bars, Fixture.Calendar};
  pb::ReplaySessionRequest Request;
  *Request.mutable_market() = RunRequest.market();
  if (RunRequest.has_stress())
    *Request.mutable_stress() = RunRequest.stress();
  *Request.mutable_journal() = Run.journal();
  Request.set_up_to_step(Run.last_step());
  Request.set_events_from_step(-1);
  Request.set_include_report(true);
  pb::ReplaySessionResponse Replayed;
  const grpc::Status Status =
      worker::handleReplaySession(Context, Request, &Replayed);
  ASSERT_TRUE(Status.ok()) << Status.error_message();

  ASSERT_EQ(Replayed.fills_size(), Run.fills_size());
  for (int I = 0; I < Run.fills_size(); ++I) {
    EXPECT_EQ(Replayed.fills(I).SerializeAsString(),
              Run.fills(I).SerializeAsString())
        << I;
  }
  ASSERT_EQ(Replayed.orders_size(), Run.orders_size());
  for (int I = 0; I < Run.orders_size(); ++I) {
    EXPECT_EQ(Replayed.orders(I).SerializeAsString(),
              Run.orders(I).SerializeAsString())
        << I;
  }
  EXPECT_EQ(Replayed.report().final_equity().SerializeAsString(),
            Run.report().final_equity().SerializeAsString());

  // Портфель на промежуточном шаге - тот же путь, что у ручной сессии: записи
  // журнала до курсора включительно.
  Request.clear_journal();
  for (const pb::JournalEntry &E : Run.journal()) {
    if (E.step() <= 4)
      *Request.add_journal() = E;
  }
  Request.set_up_to_step(4);
  Request.set_include_report(false);
  pb::ReplaySessionResponse Middle;
  ASSERT_TRUE(worker::handleReplaySession(Context, Request, &Middle).ok());
  EXPECT_EQ(Middle.last_step(), 4);
  for (const pb::Fill &F : Middle.fills())
    EXPECT_LE(F.step(), 4);
}

TEST_F(RunBacktestRpc, JournalReplaysToSameTradesAndOrders) {
  expectJournalReplaysRun(Fixture, Request, result());
}

TEST_F(RunBacktestRpc, JournalReplaysStressedRun) {
  setStress(3, "20", 2);
  const pb::RunResult Stressed = result();
  expectJournalReplaysRun(Fixture, Request, Stressed);
  // Без стресса тот же журнал даёт другие цены: стресс в запросе
  // воспроизведения обязателен.
  worker::Context Context{Fixture.Bars, Fixture.Calendar};
  pb::ReplaySessionRequest Replay;
  *Replay.mutable_market() = Request.market();
  *Replay.mutable_journal() = Stressed.journal();
  Replay.set_up_to_step(Stressed.last_step());
  Replay.set_events_from_step(-1);
  pb::ReplaySessionResponse Plain;
  ASSERT_TRUE(worker::handleReplaySession(Context, Replay, &Plain).ok());
  std::string A, B;
  for (const pb::Fill &F : Plain.fills())
    A += F.SerializeAsString();
  for (const pb::Fill &F : Stressed.fills())
    B += F.SerializeAsString();
  EXPECT_NE(A, B);
}

TEST_F(RunBacktestRpc, JournalReplaysNaiveRun) {
  Request.mutable_market()->set_naive(true);
  expectJournalReplaysRun(Fixture, Request, result());
}
