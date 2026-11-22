#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
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

class AnalyzeDivergencesRpc : public ::testing::Test {
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

  grpc::Status analyze(const pb::DivergenceRequest &Req,
                       pb::DivergenceResponse *Resp) {
    grpc::ClientContext ClientCtx;
    return Stub->AnalyzeDivergences(&ClientCtx, Req, Resp);
  }

  void addSeriesWithBenchAndRuonia(const std::string &Snapshot,
                                   const std::string &Ticker,
                                   const std::vector<core::Bar> &Series,
                                   const std::vector<core::Bar> &Bench,
                                   const std::vector<core::TradingDay> &Cal) {
    const core::Decimal Step = core::Decimal::parse("0.01");
    Fixture.Bars.addSecurity(Snapshot, core::Security{Ticker, "Тест", 1, Step});
    Fixture.Bars.addSeries(Snapshot, Ticker, 86400, Series);
    Fixture.Bars.addSecurity(Snapshot,
                             core::Security{"IMOEX", "Индекс", 1, Step});
    Fixture.Bars.addSeries(Snapshot, "IMOEX", 86400, Bench);
    Fixture.Calendar.add(Snapshot, Cal);
    const core::Decimal Rate = core::Decimal::parse("12.0");
    std::vector<core::DailyRate> Rates;
    Rates.push_back(
        core::DailyRate{core::addCalendarDays(Cal.front().Yyyymmdd, -1), Rate});
    for (const core::TradingDay &Day : Cal) {
      Rates.push_back(core::DailyRate{Day.Yyyymmdd, Rate});
    }
    Fixture.Ruonia.add(Snapshot, Rates);
  }

  core::testing::Fixture Fixture;
  std::unique_ptr<worker::WorkerService> Service;
  std::unique_ptr<grpc::Server> Server;
  std::unique_ptr<pb::SimulationWorker::Stub> Stub;
};

pb::MarketSpec zeroCostMarket(const std::string &Snapshot,
                              const std::string &FromDate,
                              const std::string &ToDate,
                              const char *InitialCash = "10000") {
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

std::vector<core::Bar> makeBars(const std::vector<std::int32_t> &Days,
                                const std::vector<const char *> &Closes) {
  std::vector<core::Bar> Bars;
  core::Decimal Prev = core::Decimal::parse(Closes.front());
  for (std::size_t I = 0; I < Days.size(); ++I) {
    const std::int64_t OpenMs = core::moscowDayStartMs(Days[I]);
    core::Bar B;
    B.TsOpenMs = OpenMs;
    B.AvailableAtMs = OpenMs + core::kDayMs;
    B.Open = Prev;
    B.Close = core::Decimal::parse(Closes[I]);
    B.High = std::max(B.Open, B.Close);
    B.Low = std::min(B.Open, B.Close);
    B.Volume = 10000;
    B.Value = B.Close.times(B.Volume);
    Prev = B.Close;
    Bars.push_back(B);
  }
  return Bars;
}

} // namespace

TEST_F(AnalyzeDivergencesRpc, DivergencesSeqCostPercentMissedAndPortfolios) {
  const std::vector<std::int32_t> Days{20240201, 20240202, 20240205, 20240206,
                                       20240207, 20240208, 20240209};
  const std::vector<const char *> Closes{"100", "105", "110", "120",
                                         "125", "130", "135"};
  const std::vector<const char *> BenchCloses{"1000", "1010", "1020", "1030",
                                              "1040", "1050", "1060"};
  std::vector<core::TradingDay> Cal;
  for (const std::int32_t D : Days) {
    Cal.push_back(core::TradingDay{D, true});
  }
  addSeriesWithBenchAndRuonia("div-snap", "TESTA", makeBars(Days, Closes),
                              makeBars(Days, BenchCloses), Cal);

  pb::DivergenceRequest Req;
  *Req.mutable_market() =
      zeroCostMarket("div-snap", "2024-02-01", "2024-02-09", "10000");
  Req.mutable_market()->set_benchmark_ticker("IMOEX");
  *Req.add_journal() = marketOrder(0, 0, "A", pb::BUY, 1);
  *Req.add_journal() = marketOrder(1, 3, "A", pb::SELL, 1);
  Req.set_last_step(6);

  pb::AlgorithmRef *Alg1 = Req.add_algorithms();
  Alg1->set_id("alg1");
  Alg1->mutable_strategy()->set_source(R"dsl(
rule "late"
  when close == 125
  then buy 1 lots market
  because "поздно"
)dsl");

  pb::AlgorithmRef *Alg2 = Req.add_algorithms();
  Alg2->set_id("alg2");
  Alg2->mutable_strategy()->set_source(R"dsl(
param qty = 1
rule "entry"
  when position == 0 and close == 100
  then buy qty lots limit 100
  because "вход"
)dsl");
  pb::ParamValue *Ov = Alg2->mutable_strategy()->add_param_overrides();
  Ov->set_name("qty");
  Ov->set_value(3.0);

  pb::DivergenceResponse Resp;
  const grpc::Status St = analyze(Req, &Resp);
  ASSERT_TRUE(St.ok()) << St.error_message();

  ASSERT_EQ(Resp.divergences_size(), 5);
  for (int I = 0; I < Resp.divergences_size(); ++I) {
    EXPECT_EQ(Resp.divergences(I).seq(), I);
  }

  const pb::Divergence &D0 = Resp.divergences(0);
  EXPECT_EQ(D0.algorithm_id(), "alg1");
  EXPECT_EQ(D0.instrument_key(), "A");
  EXPECT_EQ(D0.type(), pb::EXCESS);
  EXPECT_EQ(D0.human_step(), 0);
  EXPECT_EQ(D0.algo_step(), -1);
  EXPECT_EQ(D0.human_lots(), 1);
  EXPECT_EQ(D0.algo_lots(), 0);
  EXPECT_EQ(D0.horizon_step(), 3);
  EXPECT_EQ(core::fromProto(D0.cost()), core::Decimal::parse("-20"));
  EXPECT_EQ(core::fromProto(D0.cost_percent()), core::Decimal::parse("-0.2"));
  EXPECT_EQ(D0.human_order_type(), pb::MARKET);
  EXPECT_EQ(D0.algo_order_type(), pb::ORDER_TYPE_UNSPECIFIED);

  const pb::Divergence &D2 = Resp.divergences(2);
  EXPECT_EQ(D2.algorithm_id(), "alg1");
  EXPECT_EQ(D2.type(), pb::MISSED);
  EXPECT_EQ(D2.human_step(), -1);
  EXPECT_EQ(D2.algo_step(), 4);
  EXPECT_EQ(D2.reason(), "поздно");
  EXPECT_EQ(D2.human_order_type(), pb::ORDER_TYPE_UNSPECIFIED);
  EXPECT_EQ(D2.algo_order_type(), pb::MARKET);

  const pb::Divergence &D3 = Resp.divergences(3);
  EXPECT_EQ(D3.algorithm_id(), "alg2");
  EXPECT_EQ(D3.type(), pb::SIZE);
  EXPECT_EQ(D3.human_lots(), 1);
  EXPECT_EQ(D3.algo_lots(), 3);
  EXPECT_EQ(D3.human_order_type(), pb::MARKET);
  EXPECT_EQ(D3.algo_order_type(), pb::LIMIT);

  ASSERT_EQ(Resp.missed_profitable_size(), 1);
  const pb::MissedProfitableTrade &M = Resp.missed_profitable(0);
  EXPECT_EQ(M.instrument_key(), "A");
  EXPECT_EQ(M.entry_step(), 1);
  EXPECT_EQ(M.exit_step(), 4);
  EXPECT_EQ(M.lots(), 1);
  EXPECT_EQ(core::fromProto(M.net_pnl()), core::Decimal::parse("20"));
  EXPECT_EQ(core::fromProto(M.entry_price()), core::Decimal::parse("100"));
  EXPECT_EQ(core::fromProto(M.exit_price()), core::Decimal::parse("120"));
  EXPECT_EQ(M.seq(), 0);
  ASSERT_EQ(M.algorithm_ids_size(), 1);
  EXPECT_EQ(M.algorithm_ids(0), "alg1");
  ASSERT_EQ(M.divergence_seqs_size(), 1);
  EXPECT_EQ(M.divergence_seqs(0), D0.seq());

  ASSERT_EQ(Resp.consensus_size(), 3);
  EXPECT_TRUE(Resp.consensus(0).human_action());
  EXPECT_EQ(Resp.consensus(0).step(), 0);
  EXPECT_EQ(Resp.consensus(0).agreeing(), 1);
  EXPECT_EQ(Resp.consensus(0).total(), 2);
  EXPECT_FALSE(Resp.consensus(2).human_action());
  EXPECT_EQ(Resp.consensus(2).step(), 4);

  ASSERT_EQ(Resp.algorithm_portfolios_size(), 2);
  EXPECT_EQ(Resp.algorithm_portfolios(0).algorithm_id(), "alg1");
  EXPECT_EQ(Resp.algorithm_portfolios(1).algorithm_id(), "alg2");
  for (const pb::AlgorithmPortfolio &Item : Resp.algorithm_portfolios()) {
    const pb::RunResult &Port = Item.result();
    EXPECT_EQ(Port.status(), pb::COMPLETED);
    EXPECT_EQ(Port.last_step(), 6);
    EXPECT_EQ(Port.total_steps(), 7);
    EXPECT_NE(Port.digest(), 0u);
    EXPECT_EQ(Port.history_size(), 0);
    ASSERT_TRUE(Port.has_report());
    EXPECT_TRUE(Port.report().metrics().has_benchmark_return());
    EXPECT_NEAR(Port.report().metrics().benchmark_return(), 0.06, 1e-12);
    EXPECT_EQ(Port.report().benchmark_equity_size(), 7);
    EXPECT_TRUE(Port.report().metrics().has_sharpe());
  }
  ASSERT_FALSE(Resp.algorithm_portfolios(1).result().orders().empty());
  EXPECT_EQ(Resp.algorithm_portfolios(1).result().orders(0).spec().lots(), 3);
}

TEST_F(AnalyzeDivergencesRpc, TimingDivergenceAndEmptyAlgorithms) {
  pb::DivergenceRequest Req;
  *Req.mutable_market() = fixtureMarket();
  *Req.add_journal() = marketOrder(0, 0, "A", pb::BUY, 1);
  Req.set_last_step(9);

  pb::DivergenceResponse EmptyResp;
  ASSERT_TRUE(analyze(Req, &EmptyResp).ok());
  EXPECT_EQ(EmptyResp.divergences_size(), 0);
  EXPECT_EQ(EmptyResp.missed_profitable_size(), 0);
  EXPECT_EQ(EmptyResp.consensus_size(), 0);
  EXPECT_EQ(EmptyResp.algorithm_portfolios_size(), 0);

  // Проверим Timing на правиле шага 2 (в пределах окна W=3).
  const std::vector<std::int32_t> Days{20240301, 20240304, 20240305, 20240306};
  const std::vector<const char *> Closes{"100", "101", "102", "105"};
  std::vector<core::TradingDay> Cal;
  for (const std::int32_t D : Days) {
    Cal.push_back(core::TradingDay{D, true});
  }
  addSeriesWithBenchAndRuonia("timing-snap", "TESTA", makeBars(Days, Closes),
                              makeBars(Days, Closes), Cal);

  pb::DivergenceRequest TimingReq;
  *TimingReq.mutable_market() =
      zeroCostMarket("timing-snap", "2024-03-01", "2024-03-06", "10000");
  *TimingReq.add_journal() = marketOrder(0, 0, "A", pb::BUY, 1);
  TimingReq.set_last_step(3);
  pb::AlgorithmRef *Alg = TimingReq.add_algorithms();
  Alg->set_id("t1");
  Alg->mutable_strategy()->set_source(R"dsl(
rule "delayed"
  when close == 102
  then buy 1 lots market
  because "задержка"
)dsl");

  pb::DivergenceResponse TimingResp;
  const grpc::Status St = analyze(TimingReq, &TimingResp);
  ASSERT_TRUE(St.ok()) << St.error_message();
  ASSERT_EQ(TimingResp.divergences_size(), 1);
  EXPECT_EQ(TimingResp.divergences(0).type(), pb::TIMING);
  EXPECT_EQ(TimingResp.divergences(0).human_step(), 0);
  EXPECT_EQ(TimingResp.divergences(0).algo_step(), 2);
}

TEST_F(AnalyzeDivergencesRpc, ValidationErrorsAndCancellation) {
  {
    pb::DivergenceRequest Req;
    pb::DivergenceResponse Resp;
    EXPECT_EQ(analyze(Req, &Resp).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
  }
  {
    pb::DivergenceRequest Req;
    *Req.mutable_market() = fixtureMarket();
    Req.set_last_step(-1);
    pb::DivergenceResponse Resp;
    EXPECT_EQ(analyze(Req, &Resp).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    Req.set_last_step(10);
    EXPECT_EQ(analyze(Req, &Resp).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
  }
  {
    pb::DivergenceRequest Req;
    *Req.mutable_market() = fixtureMarket();
    Req.set_last_step(5);
    for (int I = 0; I < 6; ++I) {
      pb::AlgorithmRef *Alg = Req.add_algorithms();
      Alg->set_id("a" + std::to_string(I));
      Alg->mutable_strategy()->set_source(
          "rule \"r\" when false then buy 1 lots market because \"no\"");
    }
    pb::DivergenceResponse Resp;
    EXPECT_EQ(analyze(Req, &Resp).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
  }
  {
    pb::DivergenceRequest Req;
    *Req.mutable_market() = fixtureMarket();
    Req.set_last_step(5);
    for (int I = 0; I < 2; ++I) {
      pb::AlgorithmRef *Alg = Req.add_algorithms();
      Alg->set_id("dup");
      Alg->mutable_strategy()->set_source(
          "rule \"r\" when false then buy 1 lots market because \"no\"");
    }
    pb::DivergenceResponse Resp;
    EXPECT_EQ(analyze(Req, &Resp).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
  }
  {
    pb::DivergenceRequest Req;
    *Req.mutable_market() = fixtureMarket();
    Req.set_last_step(5);
    pb::AlgorithmRef *Alg = Req.add_algorithms();
    Alg->set_id("");
    Alg->mutable_strategy()->set_source(
        "rule \"r\" when false then cancel because \"no\"");
    pb::DivergenceResponse Resp;
    EXPECT_EQ(analyze(Req, &Resp).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
  }
  {
    pb::DivergenceRequest Req;
    *Req.mutable_market() = fixtureMarket();
    Req.set_last_step(5);
    Req.add_algorithms()->set_id("no-strat");
    pb::DivergenceResponse Resp;
    EXPECT_EQ(analyze(Req, &Resp).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
  }
  {
    pb::DivergenceRequest Req;
    *Req.mutable_market() = fixtureMarket();
    Req.set_last_step(5);
    pb::AlgorithmRef *Alg = Req.add_algorithms();
    Alg->set_id("dup-param");
    Alg->mutable_strategy()->set_source(
        "param p = 1\nrule \"r\" when false then cancel because \"no\"");
    for (int I = 0; I < 2; ++I) {
      pb::ParamValue *Ov = Alg->mutable_strategy()->add_param_overrides();
      Ov->set_name("p");
      Ov->set_value(1.0);
    }
    pb::DivergenceResponse Resp;
    EXPECT_EQ(analyze(Req, &Resp).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
  }
  {
    pb::DivergenceRequest Req;
    *Req.mutable_market() = fixtureMarket();
    Req.set_last_step(5);
    pb::AlgorithmRef *Alg = Req.add_algorithms();
    Alg->set_id("la");
    Alg->mutable_strategy()->set_source(
        "rule \"r\" when close[-1] > close then cancel because \"la\"");
    pb::DivergenceResponse Resp;
    EXPECT_EQ(analyze(Req, &Resp).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
  }
  {
    pb::DivergenceRequest Req;
    *Req.mutable_market() = fixtureMarket();
    Req.set_last_step(5);
    *Req.add_journal() = marketOrder(1, 0, "A", pb::BUY, 1);
    *Req.add_journal() = marketOrder(1, 1, "A", pb::SELL, 1);
    pb::DivergenceResponse Resp;
    EXPECT_EQ(analyze(Req, &Resp).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
  }
  {
    pb::DivergenceRequest Req;
    *Req.mutable_market() = fixtureMarket();
    Req.mutable_market()->set_snapshot_id("missing-snap");
    Req.set_last_step(5);
    pb::DivergenceResponse Resp;
    EXPECT_EQ(analyze(Req, &Resp).error_code(),
              grpc::StatusCode::FAILED_PRECONDITION);
  }
  {
    pb::DivergenceRequest Req;
    *Req.mutable_market() = fixtureMarket();
    Req.set_last_step(5);
    pb::DivergenceResponse Resp;
    grpc::ClientContext ClientCtx;
    ClientCtx.TryCancel();
    const grpc::Status St = Stub->AnalyzeDivergences(&ClientCtx, Req, &Resp);
    EXPECT_EQ(St.error_code(), grpc::StatusCode::CANCELLED);
  }
}
