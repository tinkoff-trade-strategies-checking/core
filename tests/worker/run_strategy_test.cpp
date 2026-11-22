#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/market/calendar_source.hpp"
#include "core/market/date_utils.hpp"
#include "core/market/in_memory_bar_source.hpp"
#include "core/market/ruonia_source.hpp"
#include "core/money/decimal.hpp"
#include "core/money/proto_conv.hpp"
#include "core/report/ruonia.hpp"
#include "worker_service.hpp"
#include "worker_test_support.hpp"

namespace {

namespace pb = pac::simulation::v1;
using namespace worker_test;

class RunStrategyRpc : public ::testing::Test {
protected:
  void SetUp() override {
    Fixture = core::testing::fixture1();
    Service = std::make_unique<worker::WorkerService>(
        worker::Context{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia});
    grpc::ServerBuilder Builder;
    Builder.RegisterService(Service.get());
    Server = Builder.BuildAndStart();
    ASSERT_TRUE(Server);
    Stub = pb::SimulationWorker::NewStub(
        Server->InProcessChannel(grpc::ChannelArguments()));
  }

  void TearDown() override {
    if (Server) {
      Server->Shutdown();
    }
  }

  grpc::Status collectStream(const pb::RunStrategyRequest &Req,
                             std::vector<pb::RunProgress> &Progress,
                             std::vector<pb::RunResult> &Results) {
    grpc::ClientContext ClientCtx;
    std::unique_ptr<grpc::ClientReader<pb::RunStrategyUpdate>> Reader =
        Stub->RunStrategy(&ClientCtx, Req);
    pb::RunStrategyUpdate Update;
    while (Reader->Read(&Update)) {
      if (Update.has_progress()) {
        Progress.push_back(Update.progress());
      } else if (Update.has_result()) {
        Results.push_back(Update.result());
      }
    }
    return Reader->Finish();
  }

  void addSyntheticSeries(const std::string &Snapshot,
                          const std::string &Ticker,
                          const std::vector<core::Bar> &Series,
                          const std::vector<core::TradingDay> &Cal) {
    Fixture.Bars.addSecurity(
        Snapshot,
        core::Security{Ticker, "Тест", 1, core::Decimal::parse("0.01")});
    Fixture.Bars.addSeries(Snapshot, Ticker, 86400, Series);
    Fixture.Calendar.add(Snapshot, Cal);
    Fixture.Ruonia.add(
        Snapshot,
        {core::DailyRate{core::addCalendarDays(Cal.front().Yyyymmdd, -1),
                         core::Decimal::parse("10.0")}});
  }

  core::testing::Fixture Fixture;
  std::unique_ptr<worker::WorkerService> Service;
  std::unique_ptr<grpc::Server> Server;
  std::unique_ptr<pb::SimulationWorker::Stub> Stub;
};

pb::MarketSpec singleInstrMarket(const std::string &Snapshot,
                                 const std::string &FromDate,
                                 const std::string &ToDate,
                                 const char *InitialCash = "100000") {
  pb::MarketSpec Market;
  Market.set_snapshot_id(Snapshot);
  pb::Instrument *Instr = Market.add_instruments();
  Instr->set_key("A");
  Instr->set_ticker("TESTA");
  Instr->set_lot_size(1);
  *Instr->mutable_price_step() = dec("0.01");
  Market.set_timeframe(pb::D1);
  Market.set_from_date(FromDate);
  Market.set_to_date(ToDate);
  pb::CostConfig *Costs = Market.mutable_costs();
  *Costs->mutable_commission_rate() = dec("0");
  *Costs->mutable_exchange_fee_rate() = dec("0");
  *Costs->mutable_spread() = dec("0");
  *Costs->mutable_slippage_kappa() = dec("0");
  *Market.mutable_initial_cash() = dec(InitialCash);
  return Market;
}

} // namespace

TEST_F(RunStrategyRpc, CompletedRunWithProgressReportAndDeterministicDigest) {
  const std::string Source = R"dsl(
param enter_lvl = 101
param exit_lvl = 104
param qty = 2

rule "вход"
  when position == 0 and close >= enter_lvl
  then buy qty lots market
  because "вход при close={close}"

rule "выход"
  when position > 0 and close >= exit_lvl
  then sell all
  because "выход при close={close}"
)dsl";

  pb::RunStrategyRequest Req;
  *Req.mutable_market() = fixtureMarket();
  Req.mutable_strategy()->set_source(Source);

  std::vector<pb::RunProgress> Progress1;
  std::vector<pb::RunResult> Results1;
  const grpc::Status St1 = collectStream(Req, Progress1, Results1);
  ASSERT_TRUE(St1.ok()) << St1.error_message();
  ASSERT_FALSE(Progress1.empty());
  EXPECT_EQ(Progress1.back().done_steps(), 10);
  EXPECT_EQ(Progress1.back().total_steps(), 10);
  ASSERT_EQ(Results1.size(), 1u);

  const pb::RunResult &Res = Results1[0];
  EXPECT_EQ(Res.status(), pb::COMPLETED);
  EXPECT_EQ(Res.level(), pb::OK);
  EXPECT_EQ(Res.last_step(), 9);
  EXPECT_EQ(Res.total_steps(), 10);
  EXPECT_TRUE(Res.has_report());
  EXPECT_EQ(Res.report().level(), pb::OK);
  EXPECT_EQ(Res.report().equity_size(), 10);
  EXPECT_GT(Res.report().round_trips_size(), 0);
  ASSERT_GE(Res.orders_size(), 2);
  EXPECT_EQ(Res.orders(0).rule_name(), "вход");
  EXPECT_NE(Res.orders(0).because().find("вход при close="), std::string::npos);
  EXPECT_GT(Res.fills_size(), 0);
  EXPECT_GT(Res.journal_size(), 0);
  EXPECT_EQ(Res.journal(0).rule_name(), "вход");
  EXPECT_NE(Res.digest(), 0u);
  EXPECT_EQ(Res.history_size(), 0);

  std::vector<pb::RunProgress> Progress2;
  std::vector<pb::RunResult> Results2;
  ASSERT_TRUE(collectStream(Req, Progress2, Results2).ok());
  ASSERT_EQ(Results2.size(), 1u);
  EXPECT_EQ(Results2[0].digest(), Res.digest());
}

TEST_F(RunStrategyRpc, IncludeHistoryIncludesWarmupAndMultipleProgress) {
  std::vector<core::Bar> Bars;
  std::vector<core::TradingDay> Cal;
  const std::int64_t BaseMs = core::moscowDayStartMs(20240101);
  for (int I = 0; I < 65; ++I) {
    const std::int64_t OpenMs = BaseMs + static_cast<std::int64_t>(I) *
                                             core::kDayMs;
    core::Bar B;
    B.TsOpenMs = OpenMs;
    B.AvailableAtMs = OpenMs + core::kDayMs;
    B.Open = B.High = B.Low = B.Close = core::Decimal::parse("100");
    B.Volume = 10000;
    B.Value = B.Close.times(B.Volume);
    Bars.push_back(B);
    Cal.push_back(core::TradingDay{core::moscowYmd(OpenMs), true});
  }
  addSyntheticSeries("long-snap", "TESTA", Bars, Cal);

  pb::MarketSpec Market = singleInstrMarket(
      "long-snap", core::formatIsoDate(Cal[5].Yyyymmdd),
      core::formatIsoDate(Cal.back().Yyyymmdd));
  Market.set_warmup_bars(5);

  pb::RunStrategyRequest Req;
  *Req.mutable_market() = Market;
  Req.mutable_strategy()->set_source(R"dsl(
rule "покупка"
  when position == 0
  then buy 1 lots market
  because "вход"
)dsl");
  Req.set_include_history(true);

  std::vector<pb::RunProgress> Progress;
  std::vector<pb::RunResult> Results;
  const grpc::Status St = collectStream(Req, Progress, Results);
  ASSERT_TRUE(St.ok()) << St.error_message();
  ASSERT_EQ(Progress.size(), 2u);
  EXPECT_EQ(Progress[0].done_steps(), 50);
  EXPECT_EQ(Progress[0].total_steps(), 60);
  EXPECT_EQ(Progress[1].done_steps(), 60);
  EXPECT_EQ(Progress[1].total_steps(), 60);

  ASSERT_EQ(Results.size(), 1u);
  const pb::RunResult &Res = Results[0];
  EXPECT_EQ(Res.status(), pb::COMPLETED);
  EXPECT_EQ(Res.last_step(), 59);
  EXPECT_EQ(Res.total_steps(), 60);
  ASSERT_EQ(Res.history_size(), 65);
  EXPECT_EQ(Res.history(0).step(), -5);
  EXPECT_EQ(Res.history(4).step(), -1);
  EXPECT_EQ(Res.history(5).step(), 0);
  EXPECT_EQ(Res.history(64).step(), 59);
}

TEST_F(RunStrategyRpc, InvalidStrategyReturnsOkWithInvalidAndDiagnostics) {
  {
    pb::RunStrategyRequest Req;
    *Req.mutable_market() = fixtureMarket();
    Req.mutable_strategy()->set_source(
        "rule \"r\" when ) then cancel because \"x\"");
    std::vector<pb::RunProgress> Progress;
    std::vector<pb::RunResult> Results;
    const grpc::Status St = collectStream(Req, Progress, Results);
    ASSERT_TRUE(St.ok()) << St.error_message();
    ASSERT_EQ(Results.size(), 1u);
    EXPECT_EQ(Results[0].status(), pb::INVALID);
    EXPECT_EQ(Results[0].level(), pb::ERROR);
    EXPECT_EQ(Results[0].last_step(), -1);
    EXPECT_FALSE(Results[0].has_report());
    ASSERT_FALSE(Results[0].diagnostics().empty());
    EXPECT_EQ(Results[0].diagnostics(0).code(), "E-SYN");
  }
  {
    pb::RunStrategyRequest Req;
    *Req.mutable_market() = fixtureMarket();
    Req.mutable_strategy()->set_source(
        "rule \"la\" when close[-1] > close then cancel because \"la\"");
    std::vector<pb::RunProgress> Progress;
    std::vector<pb::RunResult> Results;
    const grpc::Status St = collectStream(Req, Progress, Results);
    ASSERT_TRUE(St.ok()) << St.error_message();
    ASSERT_EQ(Results.size(), 1u);
    EXPECT_EQ(Results[0].status(), pb::INVALID);
    EXPECT_EQ(Results[0].level(), pb::ERROR);
    ASSERT_FALSE(Results[0].diagnostics().empty());
    EXPECT_EQ(Results[0].diagnostics(0).code(), "E-LA01");
  }
}

TEST_F(RunStrategyRpc, BankruptStopHaltsEarlyAndRespectsInv1) {
  const std::int32_t Days[] = {20240201, 20240202, 20240205};
  const char *Opens[] = {"10", "100", "50"};
  const char *Closes[] = {"10", "1", "50"};
  std::vector<core::Bar> Bars;
  std::vector<core::TradingDay> Cal;
  for (int I = 0; I < 3; ++I) {
    const std::int64_t OpenMs = core::moscowDayStartMs(Days[I]);
    core::Bar B;
    B.TsOpenMs = OpenMs;
    B.AvailableAtMs = OpenMs + core::kDayMs;
    B.Open = core::Decimal::parse(Opens[I]);
    B.Close = core::Decimal::parse(Closes[I]);
    B.High = std::max(B.Open, B.Close);
    B.Low = std::min(B.Open, B.Close);
    B.Volume = 100000;
    B.Value = B.Close.times(B.Volume);
    Bars.push_back(B);
    Cal.push_back(core::TradingDay{Days[I], true});
  }
  addSyntheticSeries("bankrupt-snap", "TESTA", Bars, Cal);

  pb::RunStrategyRequest Req;
  *Req.mutable_market() =
      singleInstrMarket("bankrupt-snap", "2024-02-01", "2024-02-05", "1000");
  Req.mutable_strategy()->set_source(R"dsl(
rule "all_in"
  when position == 0
  then buy 90 lots market
  because "максимум"
)dsl");
  Req.set_include_history(true);

  std::vector<pb::RunProgress> Progress;
  std::vector<pb::RunResult> Results;
  const grpc::Status St = collectStream(Req, Progress, Results);
  ASSERT_TRUE(St.ok()) << St.error_message();
  ASSERT_EQ(Results.size(), 1u);
  const pb::RunResult &Res = Results[0];
  EXPECT_EQ(Res.status(), pb::BANKRUPT_STOP);
  EXPECT_EQ(Res.last_step(), 1);
  EXPECT_EQ(Res.total_steps(), 3);
  EXPECT_TRUE(Res.has_report());
  ASSERT_EQ(Res.history_size(), 2);
  EXPECT_EQ(Res.history(1).step(), 1);
}

TEST_F(RunStrategyRpc, ParamOverridesValidationAndCancellation) {
  {
    pb::RunStrategyRequest Req;
    *Req.mutable_market() = fixtureMarket();
    Req.mutable_strategy()->set_source(R"dsl(
param qty = 1
rule "вход"
  when position == 0 and close >= 100
  then buy qty lots market
  because "лот {qty}"
)dsl");
    pb::ParamValue *Ov = Req.mutable_strategy()->add_param_overrides();
    Ov->set_name("qty");
    Ov->set_value(3.0);

    std::vector<pb::RunProgress> Progress;
    std::vector<pb::RunResult> Results;
    const grpc::Status St = collectStream(Req, Progress, Results);
    ASSERT_TRUE(St.ok()) << St.error_message();
    ASSERT_EQ(Results.size(), 1u);
    ASSERT_FALSE(Results[0].orders().empty());
    EXPECT_EQ(Results[0].orders(0).spec().lots(), 3);
  }
  {
    pb::RunStrategyRequest Req;
    *Req.mutable_market() = fixtureMarket();
    Req.mutable_strategy()->set_source(
        "param qty = 1\nrule \"r\" when true then buy qty lots market "
        "because \"ok\"");
    for (int I = 0; I < 2; ++I) {
      pb::ParamValue *Ov = Req.mutable_strategy()->add_param_overrides();
      Ov->set_name("qty");
      Ov->set_value(static_cast<double>(I + 1));
    }
    std::vector<pb::RunProgress> Progress;
    std::vector<pb::RunResult> Results;
    const grpc::Status St = collectStream(Req, Progress, Results);
    EXPECT_EQ(St.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  }
  {
    pb::RunStrategyRequest Req;
    *Req.mutable_market() = fixtureMarket();
    Req.mutable_strategy()->set_source(
        "rule \"r\" when true then cancel because \"ok\"");
    grpc::ClientContext ClientCtx;
    ClientCtx.TryCancel();
    std::unique_ptr<grpc::ClientReader<pb::RunStrategyUpdate>> Reader =
        Stub->RunStrategy(&ClientCtx, Req);
    pb::RunStrategyUpdate Update;
    while (Reader->Read(&Update)) {
    }
    EXPECT_EQ(Reader->Finish().error_code(), grpc::StatusCode::CANCELLED);
  }
}
