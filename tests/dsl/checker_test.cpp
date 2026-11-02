#include "core/dsl/checker.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "core/dsl/parser.hpp"

namespace core::dsl {
namespace {

std::string readFile(const std::string& relativePath) {
  std::ifstream in(std::string(CORE_TESTDATA_DIR) + "/" + relativePath, std::ios::binary);
  EXPECT_TRUE(in.good()) << relativePath;
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

CheckResult checkSource(const std::string& source) {
  const ParseResult parsed = parse(source);
  EXPECT_TRUE(parsed.program.has_value()) << source;
  if (!parsed.program) return {};
  return check(*parsed.program);
}

// Программа из одного правила с заданным условием; параметры - перед правилом.
CheckResult checkWhen(const std::string& when, const std::string& params = "",
                      const std::string& action = "buy 1 lots", const std::string& because = "\"причина\"") {
  return checkSource(params + "rule \"r\" when " + when + " then " + action + " because " + because);
}

using Codes = std::vector<std::string>;

Codes codes(const CheckResult& result) {
  Codes out;
  for (const Diagnostic& d : result.diagnostics) out.push_back(d.code);
  return out;
}

bool has(const CheckResult& result, const std::string& code) {
  for (const Diagnostic& d : result.diagnostics) {
    if (d.code == code) return true;
  }
  return false;
}

const ResolvedParam* findParam(const CheckResult& result, const std::string& name) {
  for (const ResolvedParam& p : result.params) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

TEST(Checker, ReferenceIsOk) {
  const CheckResult result = checkSource(readFile("dsl/reference.lab"));
  EXPECT_EQ(result.level, Level::Ok);
  EXPECT_TRUE(result.diagnostics.empty());
  ASSERT_EQ(result.params.size(), 6u);
  const ResolvedParam* fast = findParam(result, "fast");
  ASSERT_NE(fast, nullptr);
  EXPECT_DOUBLE_EQ(fast->value, 20);
  EXPECT_EQ(fast->min, 5);
  EXPECT_EQ(fast->max, 50);
  EXPECT_EQ(fast->step, 5);
  EXPECT_TRUE(fast->integer);
  EXPECT_TRUE(findParam(result, "slow")->integer);
  EXPECT_TRUE(findParam(result, "rsi_n")->integer);
  EXPECT_FALSE(findParam(result, "rsi_n")->min.has_value());
  EXPECT_FALSE(findParam(result, "stop_pct")->integer);  // не стоит в целочисленной позиции
  EXPECT_FALSE(findParam(result, "size_pct")->integer);
}

TEST(Checker, NegativeOffsetIsLookAhead) {
  const CheckResult result = checkWhen("close[-1] > close");
  EXPECT_EQ(result.level, Level::Error);
  ASSERT_EQ(codes(result), Codes{"E-LA01"});
  EXPECT_EQ(result.diagnostics[0].level, Level::Error);
  EXPECT_EQ(result.diagnostics[0].range.startLine, 1);
}

TEST(Checker, NegativeOffsetOfIndicatorAndParamIsLookAhead) {
  EXPECT_TRUE(has(checkWhen("sma(close, 5)[-2] > 1"), "E-LA01"));
  EXPECT_TRUE(has(checkWhen("close[-k] > 1", "param k = 1\n"), "E-LA01"));
  EXPECT_FALSE(has(checkWhen("close[-0] > close"), "E-LA01"));  // -0 - это 0
}

TEST(Checker, ParamOffsetWithNegativeRange) {
  const CheckResult result = checkWhen("close[k] > close", "param k = 1 range -2..3 step 1\n");
  EXPECT_EQ(result.level, Level::Error);
  ASSERT_EQ(codes(result), Codes{"E-LA02"});
}

TEST(Checker, ParamOffsetWithNegativeDefaultWithoutRange) {
  EXPECT_TRUE(has(checkWhen("close[k] > close", "param k = -1\n"), "E-LA02"));
}

TEST(Checker, ParamOffsetOk) {
  const CheckResult result = checkWhen("close[k] > close", "param k = 1 range 0..5 step 1\n");
  EXPECT_EQ(result.level, Level::Ok);
  EXPECT_TRUE(findParam(result, "k")->integer);
}

TEST(Checker, NonIntegerParamOffset) {
  EXPECT_EQ(codes(checkWhen("close[k] > close", "param k = 1 range 0..3 step 0.5\n")), Codes{"E-TYPE"});
}

TEST(Checker, OffsetLimit) {
  EXPECT_EQ(checkWhen("close[5000] > close").level, Level::Ok);
  EXPECT_EQ(codes(checkWhen("close[5001] > close")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("close[k] > close", "param k = 1 range 0..6000 step 1\n")), Codes{"E-TYPE"});
}

TEST(Checker, OffsetOnlyOnSeriesAndIndicators) {
  EXPECT_EQ(checkWhen("sma(close, 5)[1] > close").level, Level::Ok);
  EXPECT_EQ(checkWhen("crossover(close, open)[1]").level, Level::Ok);
  EXPECT_EQ(codes(checkWhen("position[1] > 0")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("abs(close)[1] > 0")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("close[close] > open")), Codes{"E-TYPE"});
}

TEST(Checker, AbsolutePriceLevel) {
  const CheckResult absolute = checkWhen("close > 271.35");
  EXPECT_EQ(absolute.level, Level::Warning);
  ASSERT_EQ(codes(absolute), Codes{"W-LA01"});
  EXPECT_EQ(absolute.diagnostics[0].level, Level::Warning);

  EXPECT_FALSE(has(checkWhen("rsi(close, 14) > 70"), "W-LA01"));
}

TEST(Checker, AbsolutePriceLevelVariants) {
  EXPECT_TRUE(has(checkWhen("271.35 < close[1]"), "W-LA01"));
  EXPECT_TRUE(has(checkWhen("sma(close, 10) >= 100 * 2"), "W-LA01"));
  EXPECT_TRUE(has(checkWhen("lowest(low, 5) < level", "param level = 250\n"), "W-LA01"));
  EXPECT_TRUE(has(checkWhen("high == -(3 + 4)"), "W-LA01"));
  EXPECT_FALSE(has(checkWhen("close > sma(close, 10)"), "W-LA01"));
  EXPECT_FALSE(has(checkWhen("close > entry_price * 1.05"), "W-LA01"));
  EXPECT_FALSE(has(checkWhen("volume > 1000"), "W-LA01"));
  EXPECT_FALSE(has(checkWhen("sma(volume, 5) > 1000"), "W-LA01"));
  EXPECT_FALSE(has(checkWhen("close[1] > close"), "W-LA01"));
  EXPECT_FALSE(has(checkWhen("1 < 2"), "W-LA01"));
}

TEST(Checker, WarningDoesNotHideError) {
  const CheckResult result = checkWhen("close > 271.35 and close[-1] > 1");
  EXPECT_EQ(result.level, Level::Error);
  EXPECT_TRUE(has(result, "W-LA01"));
  EXPECT_TRUE(has(result, "E-LA01"));
}

TEST(Checker, NoNewsAccess) {
  const CheckResult result = checkWhen("news > 0");
  EXPECT_EQ(result.level, Level::Error);
  ASSERT_EQ(codes(result), Codes{"E-TYPE"});
  EXPECT_NE(result.diagnostics[0].message.find("неизвестное имя `news`"), std::string::npos)
      << result.diagnostics[0].message;
  // и функция новостей тоже неизвестна
  EXPECT_EQ(codes(checkWhen("news_sentiment(close) > 0")), Codes{"E-TYPE"});
}

TEST(Checker, EmptyBecause) {
  const CheckResult result = checkWhen("close > open", "", "buy 1 lots", "\"\"");
  EXPECT_EQ(result.level, Level::Error);
  EXPECT_EQ(codes(result), Codes{"E-BEC"});
  EXPECT_EQ(codes(checkWhen("close > open", "", "buy 1 lots", "\"   \"")), Codes{"E-BEC"});
  EXPECT_EQ(checkWhen("close > open", "", "buy 1 lots", "\"{close}\"").level, Level::Ok);
}

TEST(Checker, BecauseSubstitutionsAreChecked) {
  EXPECT_EQ(codes(checkWhen("close > open", "", "buy 1 lots", "\"x {news}\"")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("close > open", "", "buy 1 lots", "\"x {close[-1]}\"")), Codes{"E-LA01"});
}

TEST(Checker, BuyAllIsError) {
  const CheckResult result = checkWhen("close > open", "", "buy all");
  EXPECT_EQ(result.level, Level::Error);
  EXPECT_EQ(codes(result), Codes{"E-TYPE"});
  EXPECT_EQ(checkWhen("close > open", "", "sell all").level, Level::Ok);
}

TEST(Checker, NonIntegerPeriod) {
  const CheckResult result = checkWhen("sma(close, 2.5) > close");
  EXPECT_EQ(result.level, Level::Error);
  EXPECT_EQ(codes(result), Codes{"E-TYPE"});
}

TEST(Checker, PeriodLimits) {
  EXPECT_EQ(checkWhen("sma(close, 5000) > close").level, Level::Ok);
  EXPECT_EQ(codes(checkWhen("sma(close, 5001) > close")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma(close, 0) > close")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma(close, 1 + 1) > close")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma(close, close) > close")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("atr(99999999999) > 0")), Codes{"E-TYPE"});
}

TEST(Checker, PeriodParam) {
  EXPECT_EQ(checkWhen("sma(close, n) > close", "param n = 5 range 1..50 step 1\n").level, Level::Ok);
  EXPECT_EQ(codes(checkWhen("sma(close, n) > close", "param n = 5 range 0..50 step 1\n")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma(close, n) > close", "param n = 2.5\n")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma(close, n) > close", "param n = 5 range 1..9000 step 1\n")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma(close, nope) > close")), Codes{"E-TYPE"});
}

TEST(Checker, ParamDeclarations) {
  const std::string rule = "rule \"r\" when close > open then buy 1 lots because \"x\"";
  EXPECT_EQ(codes(checkSource("param a = 1\nparam a = 2\n" + rule)), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkSource("param close = 1\n" + rule)), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkSource("param sma = 1\n" + rule)), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkSource("param entry_price = 1\n" + rule)), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkSource("param a = 7 range 1..5 step 1\n" + rule)), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkSource("param a = 3 range 5..1 step 1\n" + rule)), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkSource("param a = 3 range 1..5 step 0\n" + rule)), Codes{"E-TYPE"});
  EXPECT_EQ(checkSource("param a = 5 range 1..5 step 2\n" + rule).level, Level::Ok);
}

TEST(Checker, DuplicateParamResolvedOnce) {
  const CheckResult result =
      checkSource("param a = 1\nparam a = 2\nrule \"r\" when close > open then buy 1 lots because \"x\"");
  ASSERT_EQ(result.params.size(), 1u);
  EXPECT_DOUBLE_EQ(result.params[0].value, 1);
}

TEST(Checker, ConditionTypes) {
  EXPECT_EQ(codes(checkWhen("close + 1")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("close and open")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("not close")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("-(close > open) > 1")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("(close > open) + 1 > 1")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("(close > open) > 1")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("close == (open > high)")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma(close > open, 3) > 1")), Codes{"E-TYPE"});
  EXPECT_EQ(checkWhen("true").level, Level::Ok);
  EXPECT_EQ(checkWhen("(close > open) == true and not false").level, Level::Ok);
}

TEST(Checker, NaFitsAnyType) {
  EXPECT_EQ(checkWhen("na").level, Level::Ok);
  EXPECT_EQ(checkWhen("close > na").level, Level::Ok);
  EXPECT_EQ(checkWhen("close + na > 1").level, Level::Ok);
  EXPECT_EQ(checkWhen("na and true").level, Level::Ok);
  EXPECT_EQ(checkWhen("close > open", "", "buy na lots limit na").level, Level::Ok);
}

TEST(Checker, ActionTypes) {
  EXPECT_EQ(codes(checkWhen("close > open", "", "buy (close > open) lots")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("close > open", "", "buy 1 lots limit (close > open)")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("close > open", "", "buy 1 lots stop close[-1]")), Codes{"E-LA01"});
  EXPECT_EQ(checkWhen("close > open", "", "buy 10 % equity limit close * 0.99 day").level, Level::Ok);
  EXPECT_EQ(checkWhen("close > open", "", "cancel").level, Level::Ok);
}

TEST(Checker, FunctionSignatures) {
  EXPECT_EQ(codes(checkWhen("foo(close) > 1")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma(close) > open")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("atr(14, 2) > 1")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("close(1) > 1")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma > 1")), Codes{"E-TYPE"});
  EXPECT_EQ(checkWhen("min(max(close, open), abs(sqrt(volume))) > atr(14) + stdev(close, 5) + change(close, 2)")
                .level,
            Level::Ok);
}

TEST(Checker, PortfolioNamesAreIdentifiers) {
  EXPECT_EQ(checkWhen("position > 0 and cash > equity and bars_in_position > 3 and entry_price < close").level,
            Level::Ok);
}

TEST(Checker, DiagnosticsAreInTextOrderWithRanges) {
  const CheckResult result = checkSource(
      "rule \"a\" when close > open then buy 1 lots because \"\"\n"
      "rule \"b\" when news > 0 then buy 1 lots because \"x\"");
  ASSERT_EQ(result.diagnostics.size(), 2u);
  EXPECT_EQ(result.diagnostics[0].code, "E-BEC");
  EXPECT_EQ(result.diagnostics[0].range.startLine, 1);
  EXPECT_EQ(result.diagnostics[1].code, "E-TYPE");
  EXPECT_EQ(result.diagnostics[1].range.startLine, 2);
  EXPECT_EQ(result.diagnostics[1].range.startColumn, 15);
  EXPECT_EQ(result.diagnostics[1].range.endColumn, 19);
}

TEST(Checker, OverallLevel) {
  EXPECT_EQ(overallLevel({}), Level::Ok);
  Diagnostic warning;
  warning.level = Level::Warning;
  Diagnostic error;
  error.level = Level::Error;
  EXPECT_EQ(overallLevel({warning}), Level::Warning);
  EXPECT_EQ(overallLevel({warning, error, warning}), Level::Error);
  EXPECT_EQ(overallLevel({error}), Level::Error);
}

}  // namespace
}  // namespace core::dsl
