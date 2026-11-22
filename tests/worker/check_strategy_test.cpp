#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <limits>
#include <memory>
#include <string>

#include "convert/market.hpp"
#include "core/engine/session_engine.hpp"
#include "core/market/calendar_source.hpp"
#include "core/market/in_memory_bar_source.hpp"
#include "core/market/ruonia_source.hpp"
#include "worker_service.hpp"

namespace {

namespace pb = pac::simulation::v1;

const pb::Diagnostic *findDiag(const pb::CheckStrategyResponse &Resp,
                               const std::string &Code) {
  for (const pb::Diagnostic &Diag : Resp.diagnostics()) {
    if (Diag.code() == Code)
      return &Diag;
  }
  return nullptr;
}

const pb::ParamDecl *findParam(const pb::CheckStrategyResponse &Resp,
                               const std::string &Name) {
  for (const pb::ParamDecl &Param : Resp.params()) {
    if (Param.name() == Name)
      return &Param;
  }
  return nullptr;
}

class CheckStrategyRpc : public ::testing::Test {
protected:
  void SetUp() override {
    Service = std::make_unique<worker::WorkerService>(
        worker::Context{Bars, Calendar, Ruonia});
    grpc::ServerBuilder Builder;
    Builder.RegisterService(Service.get());
    Server = Builder.BuildAndStart();
    ASSERT_TRUE(Server);
    Stub = pb::SimulationWorker::NewStub(
        Server->InProcessChannel(grpc::ChannelArguments()));
  }

  void TearDown() override {
    if (Server)
      Server->Shutdown();
  }

  core::InMemoryBarSource Bars;
  core::InMemoryCalendarSource Calendar;
  core::InMemoryRuoniaSource Ruonia;
  std::unique_ptr<worker::WorkerService> Service;
  std::unique_ptr<grpc::Server> Server;
  std::unique_ptr<pb::SimulationWorker::Stub> Stub;
};

} // namespace

TEST_F(CheckStrategyRpc, ValidStrategyAndParamMetadataViaRpc) {
  const std::string Source = R"dsl(
param fast = 20 range 5..50 step 5
param thresh = 1.5 range 0.5..5.0 step 0.25
param rsi_n = 14
param rsi_lim = 70
param qty = 10

rule "вход"
  when change(close, fast) > thresh and rsi(close, rsi_n) < rsi_lim
  then buy qty lots
  because "Δ({fast}) > {thresh}"
)dsl";

  pb::CheckStrategyRequest Req;
  Req.set_source(Source);
  pb::CheckStrategyResponse Resp;
  grpc::ClientContext ClientCtx;
  const grpc::Status Status = Stub->CheckStrategy(&ClientCtx, Req, &Resp);
  ASSERT_TRUE(Status.ok()) << Status.error_message();
  EXPECT_EQ(Resp.lookahead_level(), pb::OK);
  EXPECT_EQ(Resp.diagnostics_size(), 0);
  ASSERT_EQ(Resp.params_size(), 5);

  const pb::ParamDecl *Fast = findParam(Resp, "fast");
  ASSERT_NE(Fast, nullptr);
  EXPECT_DOUBLE_EQ(Fast->default_value(), 20.0);
  ASSERT_TRUE(Fast->has_range_min());
  EXPECT_DOUBLE_EQ(Fast->range_min(), 5.0);
  ASSERT_TRUE(Fast->has_range_max());
  EXPECT_DOUBLE_EQ(Fast->range_max(), 50.0);
  ASSERT_TRUE(Fast->has_step());
  EXPECT_EQ(Fast->step(), "5");
  EXPECT_TRUE(Fast->integer());

  const pb::ParamDecl *Thresh = findParam(Resp, "thresh");
  ASSERT_NE(Thresh, nullptr);
  EXPECT_DOUBLE_EQ(Thresh->default_value(), 1.5);
  ASSERT_TRUE(Thresh->has_step());
  EXPECT_EQ(Thresh->step(), "0.25");
  EXPECT_FALSE(Thresh->integer());

  const pb::ParamDecl *RsiN = findParam(Resp, "rsi_n");
  ASSERT_NE(RsiN, nullptr);
  EXPECT_FALSE(RsiN->has_range_min());
  EXPECT_FALSE(RsiN->has_range_max());
  EXPECT_FALSE(RsiN->has_step());
  EXPECT_TRUE(RsiN->integer());
}

TEST(CheckStrategyHandler, LinterL001Has1BasedRangeAndFixes) {
  pb::CheckStrategyRequest Req;
  Req.set_source(
      "rule \"r\" when change(close, 20) > 5 then cancel because \"ok\"");
  pb::CheckStrategyResponse Resp;
  const grpc::Status Status = worker::handleCheckStrategy(Req, &Resp);
  ASSERT_TRUE(Status.ok()) << Status.error_message();
  EXPECT_EQ(Resp.lookahead_level(), pb::OK);

  const pb::Diagnostic *L001 = findDiag(Resp, "L001");
  ASSERT_NE(L001, nullptr);
  EXPECT_EQ(L001->level(), pb::WARNING);
  EXPECT_GE(L001->start_line(), 1);
  EXPECT_GE(L001->start_column(), 1);
  EXPECT_GE(L001->end_line(), L001->start_line());
  ASSERT_GT(L001->fix_size(), 0);
  EXPECT_GT(L001->fix(0).edits_size(), 0);
}

TEST(CheckStrategyHandler, SyntaxTypeBecauseAndLookaheadDiagnostics) {
  {
    pb::CheckStrategyRequest Req;
    Req.set_source("rule \"r\" when ) then cancel because \"x\"");
    pb::CheckStrategyResponse Resp;
    ASSERT_TRUE(worker::handleCheckStrategy(Req, &Resp).ok());
    const pb::Diagnostic *Diag = findDiag(Resp, "E-SYN");
    ASSERT_NE(Diag, nullptr);
    EXPECT_EQ(Diag->level(), pb::ERROR);
    EXPECT_GE(Diag->start_line(), 1);
  }
  {
    pb::CheckStrategyRequest Req;
    Req.set_source("\xff\xfe");
    pb::CheckStrategyResponse Resp;
    ASSERT_TRUE(worker::handleCheckStrategy(Req, &Resp).ok());
    const pb::Diagnostic *Diag = findDiag(Resp, "E-SYN");
    ASSERT_NE(Diag, nullptr);
    EXPECT_EQ(Diag->start_line(), 0);
    EXPECT_EQ(Diag->start_column(), 0);
    EXPECT_EQ(Diag->end_line(), 0);
    EXPECT_EQ(Diag->end_column(), 0);
  }
  {
    pb::CheckStrategyRequest Req;
    Req.set_source(
        "rule \"r\" when unknown_var > 0 then cancel because \"ok\"");
    pb::CheckStrategyResponse Resp;
    ASSERT_TRUE(worker::handleCheckStrategy(Req, &Resp).ok());
    const pb::Diagnostic *Diag = findDiag(Resp, "E-TYPE");
    ASSERT_NE(Diag, nullptr);
    EXPECT_EQ(Diag->level(), pb::ERROR);
  }
  {
    pb::CheckStrategyRequest Req;
    Req.set_source("rule \"r\" when close > 0 then cancel");
    pb::CheckStrategyResponse Resp;
    ASSERT_TRUE(worker::handleCheckStrategy(Req, &Resp).ok());
    const pb::Diagnostic *Diag = findDiag(Resp, "E-BEC");
    ASSERT_NE(Diag, nullptr);
    EXPECT_EQ(Diag->level(), pb::ERROR);
  }
  {
    pb::CheckStrategyRequest Req;
    Req.set_source(
        "rule \"r\" when close[-1] > close then cancel because \"ok\"");
    pb::CheckStrategyResponse Resp;
    ASSERT_TRUE(worker::handleCheckStrategy(Req, &Resp).ok());
    EXPECT_EQ(Resp.lookahead_level(), pb::ERROR);
    const pb::Diagnostic *Diag = findDiag(Resp, "E-LA01");
    ASSERT_NE(Diag, nullptr);
    EXPECT_EQ(Diag->level(), pb::ERROR);
  }
  {
    pb::CheckStrategyRequest Req;
    Req.set_source("param p = 1 range -5..5 step 1\n"
                   "rule \"r\" when close[p] > 0 then cancel because \"ok\"");
    pb::CheckStrategyResponse Resp;
    ASSERT_TRUE(worker::handleCheckStrategy(Req, &Resp).ok());
    EXPECT_EQ(Resp.lookahead_level(), pb::ERROR);
    const pb::Diagnostic *Diag = findDiag(Resp, "E-LA02");
    ASSERT_NE(Diag, nullptr);
    EXPECT_EQ(Diag->level(), pb::ERROR);
  }
  {
    pb::CheckStrategyRequest Req;
    Req.set_source(
        "param n = 10\nparam lvl = 250\n"
        "rule \"r\" when sma(close, n) > lvl then cancel because \"ok\"");
    pb::CheckStrategyResponse Resp;
    ASSERT_TRUE(worker::handleCheckStrategy(Req, &Resp).ok());
    EXPECT_EQ(Resp.lookahead_level(), pb::WARNING);
    const pb::Diagnostic *Diag = findDiag(Resp, "W-LA01");
    ASSERT_NE(Diag, nullptr);
    EXPECT_EQ(Diag->level(), pb::WARNING);
  }
}

TEST(CheckStrategyHandler, ParamOverridesValidInvalidAndDuplicate) {
  const std::string Source =
      "param n = 10 range 5..20 step 5\n"
      "param lvl = 70\n"
      "rule \"r\" when rsi(close, n) > lvl then cancel because \"ok\"";

  {
    pb::CheckStrategyRequest Req;
    Req.set_source(Source);
    pb::ParamValue *Ov = Req.add_param_overrides();
    Ov->set_name("n");
    Ov->set_value(15.0);
    pb::CheckStrategyResponse Resp;
    ASSERT_TRUE(worker::handleCheckStrategy(Req, &Resp).ok());
    EXPECT_EQ(Resp.diagnostics_size(), 0);
  }
  {
    pb::CheckStrategyRequest Req;
    Req.set_source(Source);
    pb::ParamValue *Ov = Req.add_param_overrides();
    Ov->set_name("missing");
    Ov->set_value(1.0);
    pb::CheckStrategyResponse Resp;
    ASSERT_TRUE(worker::handleCheckStrategy(Req, &Resp).ok());
    const pb::Diagnostic *Diag = findDiag(Resp, "E-TYPE");
    ASSERT_NE(Diag, nullptr);
    EXPECT_EQ(Diag->start_line(), 0);
    EXPECT_EQ(Diag->start_column(), 0);
    EXPECT_EQ(Diag->end_line(), 0);
    EXPECT_EQ(Diag->end_column(), 0);
  }
  {
    pb::CheckStrategyRequest Req;
    Req.set_source(Source);
    pb::ParamValue *Ov = Req.add_param_overrides();
    Ov->set_name("n");
    Ov->set_value(std::numeric_limits<double>::quiet_NaN());
    pb::CheckStrategyResponse Resp;
    ASSERT_TRUE(worker::handleCheckStrategy(Req, &Resp).ok());
    const pb::Diagnostic *Diag = findDiag(Resp, "E-TYPE");
    ASSERT_NE(Diag, nullptr);
    EXPECT_EQ(Diag->start_line(), 0);
    EXPECT_EQ(Diag->start_column(), 0);
  }
  {
    pb::CheckStrategyRequest Req;
    Req.set_source(Source);
    pb::ParamValue *Ov1 = Req.add_param_overrides();
    Ov1->set_name("n");
    Ov1->set_value(10.0);
    pb::ParamValue *Ov2 = Req.add_param_overrides();
    Ov2->set_name("n");
    Ov2->set_value(15.0);
    pb::CheckStrategyResponse Resp;
    const grpc::Status Status = worker::handleCheckStrategy(Req, &Resp);
    EXPECT_EQ(Status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  }
}

TEST(WorkerStatusMapping, IntegrityViolationMapsToInternalErt01) {
  grpc::Status Mapped;
  try {
    throw core::IntegrityViolation("исполнение не на текущем шаге");
  } catch (...) {
    Mapped = worker::statusFromCurrentException();
  }
  EXPECT_EQ(Mapped.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(Mapped.error_message().rfind("E-RT01", 0), 0U)
      << Mapped.error_message();
}
