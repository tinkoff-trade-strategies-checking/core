#include "core/dsl/checker.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "core/dsl/parser.hpp"

namespace core::dsl {
namespace {

std::string readFile(const std::string &RelativePath) {
  std::ifstream In(std::string(CORE_TESTDATA_DIR) + "/" + RelativePath,
                   std::ios::binary);
  EXPECT_TRUE(In.good()) << RelativePath;
  std::ostringstream Out;
  Out << In.rdbuf();
  return Out.str();
}

CheckResult checkSource(const std::string &Source) {
  const ParseResult Parsed = parse(Source);
  EXPECT_TRUE(Parsed.Ast.has_value()) << Source;
  if (!Parsed.Ast)
    return {};
  return check(*Parsed.Ast);
}

// Программа из одного правила с заданным условием; параметры - перед правилом.
CheckResult checkWhen(const std::string &When, const std::string &Params = "",
                      const std::string &Action = "buy 1 lots",
                      const std::string &Because = "\"причина\"") {
  return checkSource(Params + "rule \"r\" when " + When + " then " + Action +
                     " because " + Because);
}

using Codes = std::vector<std::string>;

Codes codes(const CheckResult &Result) {
  Codes Out;
  for (const Diagnostic &D : Result.Diagnostics)
    Out.push_back(D.Code);
  return Out;
}

bool has(const CheckResult &Result, const std::string &Code) {
  for (const Diagnostic &D : Result.Diagnostics) {
    if (D.Code == Code)
      return true;
  }
  return false;
}

const ResolvedParam *findParam(const CheckResult &Result,
                               const std::string &Name) {
  for (const ResolvedParam &P : Result.Params) {
    if (P.Name == Name)
      return &P;
  }
  return nullptr;
}

TEST(Checker, ReferenceIsOk) {
  const CheckResult Result = checkSource(readFile("dsl/reference.lab"));
  EXPECT_EQ(Result.Severity, Level::Ok);
  EXPECT_TRUE(Result.Diagnostics.empty());
  ASSERT_EQ(Result.Params.size(), 6u);
  const ResolvedParam *Fast = findParam(Result, "fast");
  ASSERT_NE(Fast, nullptr);
  EXPECT_DOUBLE_EQ(Fast->Value, 20);
  EXPECT_EQ(Fast->Min, 5);
  EXPECT_EQ(Fast->Max, 50);
  EXPECT_EQ(Fast->Step, 5);
  EXPECT_TRUE(Fast->Integer);
  EXPECT_TRUE(findParam(Result, "slow")->Integer);
  EXPECT_TRUE(findParam(Result, "rsi_n")->Integer);
  EXPECT_FALSE(findParam(Result, "rsi_n")->Min.has_value());
  EXPECT_FALSE(findParam(Result, "stop_pct")
                   ->Integer); // не стоит в целочисленной позиции
  EXPECT_FALSE(findParam(Result, "size_pct")->Integer);
}

TEST(Checker, NegativeOffsetIsLookAhead) {
  const CheckResult Result = checkWhen("close[-1] > close");
  EXPECT_EQ(Result.Severity, Level::Error);
  ASSERT_EQ(codes(Result), Codes{"E-LA01"});
  EXPECT_EQ(Result.Diagnostics[0].Severity, Level::Error);
  EXPECT_EQ(Result.Diagnostics[0].Range.StartLine, 1);
}

TEST(Checker, NegativeOffsetOfIndicatorAndParamIsLookAhead) {
  EXPECT_TRUE(has(checkWhen("sma(close, 5)[-2] > 1"), "E-LA01"));
  EXPECT_TRUE(has(checkWhen("close[-k] > 1", "param k = 1\n"), "E-LA01"));
  EXPECT_FALSE(has(checkWhen("close[-0] > close"), "E-LA01")); // -0 - это 0
}

TEST(Checker, ParamOffsetWithNegativeRange) {
  const CheckResult Result =
      checkWhen("close[k] > close", "param k = 1 range -2..3 step 1\n");
  EXPECT_EQ(Result.Severity, Level::Error);
  ASSERT_EQ(codes(Result), Codes{"E-LA02"});
}

TEST(Checker, ParamOffsetWithNegativeDefaultWithoutRange) {
  EXPECT_TRUE(has(checkWhen("close[k] > close", "param k = -1\n"), "E-LA02"));
}

TEST(Checker, ParamOffsetOk) {
  const CheckResult Result =
      checkWhen("close[k] > close", "param k = 1 range 0..5 step 1\n");
  EXPECT_EQ(Result.Severity, Level::Ok);
  EXPECT_TRUE(findParam(Result, "k")->Integer);
}

TEST(Checker, NonIntegerParamOffset) {
  EXPECT_EQ(
      codes(checkWhen("close[k] > close", "param k = 1 range 0..3 step 0.5\n")),
      Codes{"E-TYPE"});
}

TEST(Checker, OffsetLimit) {
  EXPECT_EQ(checkWhen("close[5000] > close").Severity, Level::Ok);
  EXPECT_EQ(codes(checkWhen("close[5001] > close")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("close[k] > close",
                            "param k = 1 range 0..6000 step 1\n")),
            Codes{"E-TYPE"});
}

TEST(Checker, OffsetOnlyOnSeriesAndIndicators) {
  EXPECT_EQ(checkWhen("sma(close, 5)[1] > close").Severity, Level::Ok);
  EXPECT_EQ(checkWhen("crossover(close, open)[1]").Severity, Level::Ok);
  EXPECT_EQ(codes(checkWhen("position[1] > 0")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("abs(close)[1] > 0")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("close[close] > open")), Codes{"E-TYPE"});
}

TEST(Checker, AbsolutePriceLevel) {
  const CheckResult Absolute = checkWhen("close > 271.35");
  EXPECT_EQ(Absolute.Severity, Level::Warning);
  ASSERT_EQ(codes(Absolute), Codes{"W-LA01"});
  EXPECT_EQ(Absolute.Diagnostics[0].Severity, Level::Warning);

  EXPECT_FALSE(has(checkWhen("rsi(close, 14) > 70"), "W-LA01"));
}

TEST(Checker, AbsolutePriceLevelVariants) {
  EXPECT_TRUE(has(checkWhen("271.35 < close[1]"), "W-LA01"));
  EXPECT_TRUE(has(checkWhen("sma(close, 10) >= 100 * 2"), "W-LA01"));
  EXPECT_TRUE(has(checkWhen("lowest(low, 5) < level", "param level = 250\n"),
                  "W-LA01"));
  EXPECT_TRUE(has(checkWhen("high == -(3 + 4)"), "W-LA01"));
  EXPECT_FALSE(has(checkWhen("close > sma(close, 10)"), "W-LA01"));
  EXPECT_FALSE(has(checkWhen("close > entry_price * 1.05"), "W-LA01"));
  EXPECT_FALSE(has(checkWhen("volume > 1000"), "W-LA01"));
  EXPECT_FALSE(has(checkWhen("sma(volume, 5) > 1000"), "W-LA01"));
  EXPECT_FALSE(has(checkWhen("close[1] > close"), "W-LA01"));
  EXPECT_FALSE(has(checkWhen("1 < 2"), "W-LA01"));
}

TEST(Checker, WarningDoesNotHideError) {
  const CheckResult Result = checkWhen("close > 271.35 and close[-1] > 1");
  EXPECT_EQ(Result.Severity, Level::Error);
  EXPECT_TRUE(has(Result, "W-LA01"));
  EXPECT_TRUE(has(Result, "E-LA01"));
}

TEST(Checker, NoNewsAccess) {
  const CheckResult Result = checkWhen("news > 0");
  EXPECT_EQ(Result.Severity, Level::Error);
  ASSERT_EQ(codes(Result), Codes{"E-TYPE"});
  EXPECT_NE(Result.Diagnostics[0].Message.find("неизвестное имя `news`"),
            std::string::npos)
      << Result.Diagnostics[0].Message;
  // и функция новостей тоже неизвестна
  EXPECT_EQ(codes(checkWhen("news_sentiment(close) > 0")), Codes{"E-TYPE"});
}

TEST(Checker, EmptyBecause) {
  const CheckResult Result =
      checkWhen("close > open", "", "buy 1 lots", "\"\"");
  EXPECT_EQ(Result.Severity, Level::Error);
  EXPECT_EQ(codes(Result), Codes{"E-BEC"});
  EXPECT_EQ(codes(checkWhen("close > open", "", "buy 1 lots", "\"   \"")),
            Codes{"E-BEC"});
  EXPECT_EQ(checkWhen("close > open", "", "buy 1 lots", "\"{close}\"").Severity,
            Level::Ok);
}

TEST(Checker, BecauseSubstitutionsAreChecked) {
  EXPECT_EQ(codes(checkWhen("close > open", "", "buy 1 lots", "\"x {news}\"")),
            Codes{"E-TYPE"});
  EXPECT_EQ(
      codes(checkWhen("close > open", "", "buy 1 lots", "\"x {close[-1]}\"")),
      Codes{"E-LA01"});
}

TEST(Checker, BuyAllIsError) {
  const CheckResult Result = checkWhen("close > open", "", "buy all");
  EXPECT_EQ(Result.Severity, Level::Error);
  EXPECT_EQ(codes(Result), Codes{"E-TYPE"});
  EXPECT_EQ(checkWhen("close > open", "", "sell all").Severity, Level::Ok);
}

TEST(Checker, NonIntegerPeriod) {
  const CheckResult Result = checkWhen("sma(close, 2.5) > close");
  EXPECT_EQ(Result.Severity, Level::Error);
  EXPECT_EQ(codes(Result), Codes{"E-TYPE"});
}

TEST(Checker, PeriodLimits) {
  EXPECT_EQ(checkWhen("sma(close, 5000) > close").Severity, Level::Ok);
  EXPECT_EQ(codes(checkWhen("sma(close, 5001) > close")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma(close, 0) > close")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma(close, 1 + 1) > close")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma(close, close) > close")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("atr(99999999999) > 0")), Codes{"E-TYPE"});
}

TEST(Checker, PeriodParam) {
  EXPECT_EQ(
      checkWhen("sma(close, n) > close", "param n = 5 range 1..50 step 1\n")
          .Severity,
      Level::Ok);
  EXPECT_EQ(codes(checkWhen("sma(close, n) > close",
                            "param n = 5 range 0..50 step 1\n")),
            Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma(close, n) > close", "param n = 2.5\n")),
            Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma(close, n) > close",
                            "param n = 5 range 1..9000 step 1\n")),
            Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma(close, nope) > close")), Codes{"E-TYPE"});
}

TEST(Checker, ParamDeclarations) {
  const std::string Rule =
      "rule \"r\" when close > open then buy 1 lots because \"x\"";
  EXPECT_EQ(codes(checkSource("param a = 1\nparam a = 2\n" + Rule)),
            Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkSource("param close = 1\n" + Rule)), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkSource("param sma = 1\n" + Rule)), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkSource("param entry_price = 1\n" + Rule)),
            Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkSource("param a = 7 range 1..5 step 1\n" + Rule)),
            Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkSource("param a = 3 range 5..1 step 1\n" + Rule)),
            Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkSource("param a = 3 range 1..5 step 0\n" + Rule)),
            Codes{"E-TYPE"});
  EXPECT_EQ(checkSource("param a = 5 range 1..5 step 2\n" + Rule).Severity,
            Level::Ok);
}

TEST(Checker, DuplicateParamResolvedOnce) {
  const CheckResult Result =
      checkSource("param a = 1\nparam a = 2\nrule \"r\" when close > open then "
                  "buy 1 lots because \"x\"");
  ASSERT_EQ(Result.Params.size(), 1u);
  EXPECT_DOUBLE_EQ(Result.Params[0].Value, 1);
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
  EXPECT_EQ(checkWhen("true").Severity, Level::Ok);
  EXPECT_EQ(checkWhen("(close > open) == true and not false").Severity,
            Level::Ok);
}

TEST(Checker, NaFitsAnyType) {
  EXPECT_EQ(checkWhen("na").Severity, Level::Ok);
  EXPECT_EQ(checkWhen("close > na").Severity, Level::Ok);
  EXPECT_EQ(checkWhen("close + na > 1").Severity, Level::Ok);
  EXPECT_EQ(checkWhen("na and true").Severity, Level::Ok);
  EXPECT_EQ(checkWhen("close > open", "", "buy na lots limit na").Severity,
            Level::Ok);
}

TEST(Checker, ActionTypes) {
  EXPECT_EQ(codes(checkWhen("close > open", "", "buy (close > open) lots")),
            Codes{"E-TYPE"});
  EXPECT_EQ(
      codes(checkWhen("close > open", "", "buy 1 lots limit (close > open)")),
      Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("close > open", "", "buy 1 lots stop close[-1]")),
            Codes{"E-LA01"});
  EXPECT_EQ(
      checkWhen("close > open", "", "buy 10 % equity limit close * 0.99 day")
          .Severity,
      Level::Ok);
  EXPECT_EQ(checkWhen("close > open", "", "cancel").Severity, Level::Ok);
}

TEST(Checker, FunctionSignatures) {
  EXPECT_EQ(codes(checkWhen("foo(close) > 1")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma(close) > open")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("atr(14, 2) > 1")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("close(1) > 1")), Codes{"E-TYPE"});
  EXPECT_EQ(codes(checkWhen("sma > 1")), Codes{"E-TYPE"});
  EXPECT_EQ(checkWhen("min(max(close, open), abs(sqrt(volume))) > atr(14) + "
                      "stdev(close, 5) + change(close, 2)")
                .Severity,
            Level::Ok);
}

TEST(Checker, PortfolioNamesAreIdentifiers) {
  EXPECT_EQ(checkWhen("position > 0 and cash > equity and bars_in_position > 3 "
                      "and entry_price < close")
                .Severity,
            Level::Ok);
}

TEST(Checker, DiagnosticsAreInTextOrderWithRanges) {
  const CheckResult Result =
      checkSource("rule \"a\" when close > open then buy 1 lots because \"\"\n"
                  "rule \"b\" when news > 0 then buy 1 lots because \"x\"");
  ASSERT_EQ(Result.Diagnostics.size(), 2u);
  EXPECT_EQ(Result.Diagnostics[0].Code, "E-BEC");
  EXPECT_EQ(Result.Diagnostics[0].Range.StartLine, 1);
  EXPECT_EQ(Result.Diagnostics[1].Code, "E-TYPE");
  EXPECT_EQ(Result.Diagnostics[1].Range.StartLine, 2);
  EXPECT_EQ(Result.Diagnostics[1].Range.StartColumn, 15);
  EXPECT_EQ(Result.Diagnostics[1].Range.EndColumn, 19);
}

TEST(Checker, OverallLevel) {
  EXPECT_EQ(overallLevel({}), Level::Ok);
  Diagnostic Warning;
  Warning.Severity = Level::Warning;
  Diagnostic Error;
  Error.Severity = Level::Error;
  EXPECT_EQ(overallLevel({Warning}), Level::Warning);
  EXPECT_EQ(overallLevel({Warning, Error, Warning}), Level::Error);
  EXPECT_EQ(overallLevel({Error}), Level::Error);
}

} // namespace
} // namespace core::dsl
