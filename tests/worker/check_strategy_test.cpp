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

std::string readTestdata(const std::string& relativePath) {
  std::ifstream in(std::string(CORE_TESTDATA_DIR) + "/" + relativePath, std::ios::binary);
  EXPECT_TRUE(in.good()) << relativePath;
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

class CheckStrategyHandler : public ::testing::Test {
 protected:
  grpc::Status run(const std::string& source, pb::CheckStrategyResponse* response) {
    worker::Context context{bars_, calendar_};
    pb::CheckStrategyRequest request;
    request.set_source(source);
    return worker::handleCheckStrategy(context, request, response);
  }

  static std::vector<std::string> codes(const pb::CheckStrategyResponse& response) {
    std::vector<std::string> out;
    for (const auto& d : response.diagnostics()) out.push_back(d.code());
    return out;
  }

  core::InMemoryBarSource bars_;
  core::InMemoryCalendarSource calendar_;
};

std::string ruleWhen(const std::string& condition, const std::string& params = "") {
  return params + "rule \"r\" when " + condition + " then cancel because \"x\"";
}

}  // namespace

TEST_F(CheckStrategyHandler, ReferenceStrategyIsOkWithSixParams) {
  pb::CheckStrategyResponse response;
  ASSERT_TRUE(run(readTestdata("dsl/reference.lab"), &response).ok());
  EXPECT_EQ(response.level(), pb::OK);
  EXPECT_TRUE(response.diagnostics().empty());
  ASSERT_EQ(response.params_size(), 6);

  const pb::ParamDecl& fast = response.params(0);
  EXPECT_EQ(fast.name(), "fast");
  EXPECT_EQ(fast.default_value(), 20);
  EXPECT_TRUE(fast.has_range());
  EXPECT_EQ(fast.min(), 5);
  EXPECT_EQ(fast.max(), 50);
  EXPECT_EQ(fast.step(), 5);
  EXPECT_TRUE(fast.integer());

  const pb::ParamDecl& rsiN = response.params(2);
  EXPECT_EQ(rsiN.name(), "rsi_n");
  EXPECT_FALSE(rsiN.has_range());
  EXPECT_TRUE(rsiN.integer());
  EXPECT_EQ(response.params(5).name(), "size_pct");
  EXPECT_FALSE(response.params(5).integer());
}

TEST_F(CheckStrategyHandler, NegativeOffsetIsErrorLookAhead) {
  pb::CheckStrategyResponse response;
  ASSERT_TRUE(run(ruleWhen("close[-1] > close"), &response).ok());
  EXPECT_EQ(response.level(), pb::ERROR);
  ASSERT_EQ(codes(response), std::vector<std::string>{"E-LA01"});
  const pb::Diagnostic& d = response.diagnostics(0);
  EXPECT_EQ(d.level(), pb::ERROR);
  EXPECT_EQ(d.start_line(), 1);
  EXPECT_GT(d.start_column(), 0);
  EXPECT_FALSE(d.message().empty());
  EXPECT_EQ(d.fix_size(), 0);
}

TEST_F(CheckStrategyHandler, LiteralGivesL001WithTwoEditsAndLevelStaysOk) {
  const std::string source = ruleWhen("sma(close, 20) > close");
  pb::CheckStrategyResponse response;
  ASSERT_TRUE(run(source, &response).ok());
  EXPECT_EQ(response.level(), pb::OK);  // линтер на уровень не влияет (ФТ-13)
  ASSERT_EQ(codes(response), std::vector<std::string>{"L001"});
  const pb::Diagnostic& d = response.diagnostics(0);
  EXPECT_EQ(d.level(), pb::WARNING);
  const int column = static_cast<int>(source.find("20")) + 1;
  EXPECT_EQ(d.start_column(), column);
  EXPECT_EQ(d.end_column(), column + 2);
  ASSERT_EQ(d.fix_size(), 2);
  EXPECT_EQ(d.fix(0).start_line(), 1);
  EXPECT_EQ(d.fix(0).start_column(), 1);
  EXPECT_EQ(d.fix(0).end_column(), 1);
  EXPECT_EQ(d.fix(0).text(), "param sma_n = 20\n");
  EXPECT_EQ(d.fix(1).start_column(), column);
  EXPECT_EQ(d.fix(1).end_column(), column + 2);
  EXPECT_EQ(d.fix(1).text(), "sma_n");
}

TEST_F(CheckStrategyHandler, SyntaxErrorGivesOnlyThatDiagnosticAndNoParams) {
  pb::CheckStrategyResponse response;
  ASSERT_TRUE(run("param a = 20\nrule \"r\" when ??? then cancel", &response).ok());
  EXPECT_EQ(response.level(), pb::ERROR);
  ASSERT_EQ(codes(response), std::vector<std::string>{"E-SYN"});
  EXPECT_EQ(response.diagnostics(0).level(), pb::ERROR);
  EXPECT_EQ(response.diagnostics(0).start_line(), 2);
  EXPECT_EQ(response.params_size(), 0);
}

TEST_F(CheckStrategyHandler, LevelComesFromCheckEvenWithLintWarnings) {
  pb::CheckStrategyResponse response;
  ASSERT_TRUE(run(ruleWhen("close[-k] > 1 and sma(close, 20) > close", "param k = 1\n"), &response).ok());
  EXPECT_EQ(response.level(), pb::ERROR);  // E-LA01 из check
  bool hasL001 = false;
  for (const auto& d : response.diagnostics()) hasL001 = hasL001 || d.code() == "L001";
  EXPECT_TRUE(hasL001);
}

TEST_F(CheckStrategyHandler, DiagnosticsAreOrderedByPositionThenCode) {
  const std::string source =
      "rule \"r\"\n"
      "  when sma(close, 20) > close[-1]\n"
      "  then cancel\n"
      "  because \"x\"";
  pb::CheckStrategyResponse response;
  ASSERT_TRUE(run(source, &response).ok());
  ASSERT_GE(response.diagnostics_size(), 2);
  for (int i = 1; i < response.diagnostics_size(); ++i) {
    const auto& a = response.diagnostics(i - 1);
    const auto& b = response.diagnostics(i);
    EXPECT_LE(std::make_tuple(a.start_line(), a.start_column(), a.code()),
              std::make_tuple(b.start_line(), b.start_column(), b.code()));
  }
  // L001 (литерал 20) стоит левее E-LA01 (close[-1]) на той же строке.
  EXPECT_EQ(response.diagnostics(0).code(), "L001");
  EXPECT_EQ(response.diagnostics(1).code(), "E-LA01");
}

TEST_F(CheckStrategyHandler, ResponseIsDeterministic) {
  const std::string source =
      readTestdata("dsl/reference.lab") + "\n" + ruleWhen("close[-1] > sma(close, 20) and rsi(close, 9) > 30");
  pb::CheckStrategyResponse first, second;
  ASSERT_TRUE(run(source, &first).ok());
  ASSERT_TRUE(run(source, &second).ok());
  EXPECT_EQ(first.SerializeAsString(), second.SerializeAsString());
}

TEST_F(CheckStrategyHandler, PreviousResponseContentIsReplaced) {
  pb::CheckStrategyResponse response;
  response.add_diagnostics()->set_code("stale");
  response.add_params()->set_name("stale");
  ASSERT_TRUE(run(ruleWhen("close > open"), &response).ok());
  EXPECT_TRUE(response.diagnostics().empty());
  EXPECT_EQ(response.params_size(), 0);
}

// Исключения не выходят из обработчика: странный ввод даёт ответ с диагностикой, но не падение.
TEST_F(CheckStrategyHandler, NoExceptionEscapesOnHostileInput) {
  const std::vector<std::string> inputs = {
      "",
      std::string(1, '\0'),
      "\xff\xfe\xc3\x28",
      std::string(70 * 1024, 'a'),
      std::string(5000, '(') + "close" + std::string(5000, ')'),
      "rule \"r\" when " + std::string(20000, '-') + "1 > 0 then cancel because \"x\"",
      "param p = 99999999999999999999999999999999999999999999999999999999999999999999999999999999999999",
  };
  for (const std::string& source : inputs) {
    pb::CheckStrategyResponse response;
    grpc::Status status;
    ASSERT_NO_THROW(status = run(source, &response));
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.level(), pb::ERROR) << "вход длиной " << source.size();
  }
}
