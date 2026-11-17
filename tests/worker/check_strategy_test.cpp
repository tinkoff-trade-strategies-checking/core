#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "core/market/calendar_source.hpp"
#include "core/market/in_memory_bar_source.hpp"
#include "handlers.hpp"

namespace {

namespace pb = pac::simulation::v1;

std::string readTestdata(const std::string &RelativePath) {
  std::ifstream In(std::string(CORE_TESTDATA_DIR) + "/" + RelativePath,
                   std::ios::binary);
  EXPECT_TRUE(In.good()) << RelativePath;
  std::ostringstream Out;
  Out << In.rdbuf();
  return Out.str();
}

class CheckStrategyHandler : public ::testing::Test {
protected:
  grpc::Status run(const std::string &Source,
                   pb::CheckStrategyResponse *Response) {
    worker::Context Context{Bars, Calendar};
    pb::CheckStrategyRequest Request;
    Request.set_source(Source);
    return worker::handleCheckStrategy(Context, Request, Response);
  }

  static std::vector<std::string>
  codes(const pb::CheckStrategyResponse &Response) {
    std::vector<std::string> Out;
    for (const auto &D : Response.diagnostics())
      Out.push_back(D.code());
    return Out;
  }

  core::InMemoryBarSource Bars;
  core::InMemoryCalendarSource Calendar;
};

std::string ruleWhen(const std::string &Condition,
                     const std::string &Params = "") {
  return Params + "rule \"r\" when " + Condition + " then cancel because \"x\"";
}

} // namespace

TEST_F(CheckStrategyHandler, ReferenceStrategyIsOkWithSixParams) {
  pb::CheckStrategyResponse Response;
  ASSERT_TRUE(run(readTestdata("dsl/reference.lab"), &Response).ok());
  EXPECT_EQ(Response.level(), pb::OK);
  EXPECT_TRUE(Response.diagnostics().empty());
  ASSERT_EQ(Response.params_size(), 6);

  const pb::ParamDecl &Fast = Response.params(0);
  EXPECT_EQ(Fast.name(), "fast");
  EXPECT_EQ(Fast.default_value(), 20);
  EXPECT_TRUE(Fast.has_range());
  EXPECT_EQ(Fast.min(), 5);
  EXPECT_EQ(Fast.max(), 50);
  EXPECT_EQ(Fast.step(), 5);
  EXPECT_TRUE(Fast.integer());

  const pb::ParamDecl &RsiN = Response.params(2);
  EXPECT_EQ(RsiN.name(), "rsi_n");
  EXPECT_FALSE(RsiN.has_range());
  EXPECT_TRUE(RsiN.integer());
  EXPECT_EQ(Response.params(5).name(), "size_pct");
  EXPECT_FALSE(Response.params(5).integer());
}

TEST_F(CheckStrategyHandler, NegativeOffsetIsErrorLookAhead) {
  pb::CheckStrategyResponse Response;
  ASSERT_TRUE(run(ruleWhen("close[-1] > close"), &Response).ok());
  EXPECT_EQ(Response.level(), pb::ERROR);
  ASSERT_EQ(codes(Response), std::vector<std::string>{"E-LA01"});
  const pb::Diagnostic &D = Response.diagnostics(0);
  EXPECT_EQ(D.level(), pb::ERROR);
  EXPECT_EQ(D.start_line(), 1);
  EXPECT_GT(D.start_column(), 0);
  EXPECT_FALSE(D.message().empty());
  EXPECT_EQ(D.fix_size(), 0);
}

TEST_F(CheckStrategyHandler, LiteralGivesL001WithTwoEditsAndLevelStaysOk) {
  const std::string Source = ruleWhen("sma(close, 20) > close");
  pb::CheckStrategyResponse Response;
  ASSERT_TRUE(run(Source, &Response).ok());
  EXPECT_EQ(Response.level(), pb::OK); // линтер на уровень не влияет (ФТ-13)
  ASSERT_EQ(codes(Response), std::vector<std::string>{"L001"});
  const pb::Diagnostic &D = Response.diagnostics(0);
  EXPECT_EQ(D.level(), pb::WARNING);
  const int Column = static_cast<int>(Source.find("20")) + 1;
  EXPECT_EQ(D.start_column(), Column);
  EXPECT_EQ(D.end_column(), Column + 2);
  ASSERT_EQ(D.fix_size(), 2);
  EXPECT_EQ(D.fix(0).start_line(), 1);
  EXPECT_EQ(D.fix(0).start_column(), 1);
  EXPECT_EQ(D.fix(0).end_column(), 1);
  EXPECT_EQ(D.fix(0).text(), "param sma_n = 20\n");
  EXPECT_EQ(D.fix(1).start_column(), Column);
  EXPECT_EQ(D.fix(1).end_column(), Column + 2);
  EXPECT_EQ(D.fix(1).text(), "sma_n");
}

TEST_F(CheckStrategyHandler, SyntaxErrorGivesOnlyThatDiagnosticAndNoParams) {
  pb::CheckStrategyResponse Response;
  ASSERT_TRUE(
      run("param a = 20\nrule \"r\" when ??? then cancel", &Response).ok());
  EXPECT_EQ(Response.level(), pb::ERROR);
  ASSERT_EQ(codes(Response), std::vector<std::string>{"E-SYN"});
  EXPECT_EQ(Response.diagnostics(0).level(), pb::ERROR);
  EXPECT_EQ(Response.diagnostics(0).start_line(), 2);
  EXPECT_EQ(Response.params_size(), 0);
}

TEST_F(CheckStrategyHandler, LevelComesFromCheckEvenWithLintWarnings) {
  pb::CheckStrategyResponse Response;
  ASSERT_TRUE(
      run(ruleWhen("close[-k] > 1 and sma(close, 20) > close", "param k = 1\n"),
          &Response)
          .ok());
  EXPECT_EQ(Response.level(), pb::ERROR); // E-LA01 из check
  bool HasL001 = false;
  for (const auto &D : Response.diagnostics())
    HasL001 = HasL001 || D.code() == "L001";
  EXPECT_TRUE(HasL001);
}

TEST_F(CheckStrategyHandler, DiagnosticsAreOrderedByPositionThenCode) {
  const std::string Source = "rule \"r\"\n"
                             "  when sma(close, 20) > close[-1]\n"
                             "  then cancel\n"
                             "  because \"x\"";
  pb::CheckStrategyResponse Response;
  ASSERT_TRUE(run(Source, &Response).ok());
  ASSERT_GE(Response.diagnostics_size(), 2);
  for (int I = 1; I < Response.diagnostics_size(); ++I) {
    const auto &A = Response.diagnostics(I - 1);
    const auto &B = Response.diagnostics(I);
    EXPECT_LE(std::make_tuple(A.start_line(), A.start_column(), A.code()),
              std::make_tuple(B.start_line(), B.start_column(), B.code()));
  }
  // L001 (литерал 20) стоит левее E-LA01 (close[-1]) на той же строке.
  EXPECT_EQ(Response.diagnostics(0).code(), "L001");
  EXPECT_EQ(Response.diagnostics(1).code(), "E-LA01");
}

TEST_F(CheckStrategyHandler, ResponseIsDeterministic) {
  const std::string Source =
      readTestdata("dsl/reference.lab") + "\n" +
      ruleWhen("close[-1] > sma(close, 20) and rsi(close, 9) > 30");
  pb::CheckStrategyResponse First, Second;
  ASSERT_TRUE(run(Source, &First).ok());
  ASSERT_TRUE(run(Source, &Second).ok());
  EXPECT_EQ(First.SerializeAsString(), Second.SerializeAsString());
}

TEST_F(CheckStrategyHandler, PreviousResponseContentIsReplaced) {
  pb::CheckStrategyResponse Response;
  Response.add_diagnostics()->set_code("stale");
  Response.add_params()->set_name("stale");
  ASSERT_TRUE(run(ruleWhen("close > open"), &Response).ok());
  EXPECT_TRUE(Response.diagnostics().empty());
  EXPECT_EQ(Response.params_size(), 0);
}

// Исключения не выходят из обработчика: странный ввод даёт ответ с
// диагностикой, но не падение.
TEST_F(CheckStrategyHandler, NoExceptionEscapesOnHostileInput) {
  const std::vector<std::string> Inputs = {
      "",
      std::string(1, '\0'),
      "\xff\xfe\xc3\x28",
      std::string(70 * 1024, 'a'),
      std::string(5000, '(') + "close" + std::string(5000, ')'),
      "rule \"r\" when " + std::string(20000, '-') +
          "1 > 0 then cancel because \"x\"",
      "param p = "
      "999999999999999999999999999999999999999999999999999999999999999999999999"
      "99999999999999",
  };
  for (const std::string &Source : Inputs) {
    pb::CheckStrategyResponse Response;
    grpc::Status Status;
    ASSERT_NO_THROW(Status = run(Source, &Response));
    EXPECT_TRUE(Status.ok()) << Status.error_message();
    EXPECT_EQ(Response.level(), pb::ERROR) << "вход длиной " << Source.size();
  }
}
