#include "core/dsl/linter.hpp"

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

std::vector<Diagnostic> lintText(const std::string &Source) {
  const ParseResult Parsed = parse(Source);
  EXPECT_TRUE(Parsed.Ast.has_value())
      << (Parsed.Diagnostics.empty() ? "" : Parsed.Diagnostics[0].Message);
  if (!Parsed.Ast)
    return {};
  return lint(*Parsed.Ast, Source);
}

// Правило с одним условием и пустым действием: литералы тестируются в условии.
std::string ruleWhen(const std::string &Condition) {
  return "rule \"r\" when " + Condition + " then cancel because \"x\"";
}

// Диапазон первого вхождения фрагмента в однострочный текст (столбцы с 1, конец
// исключающий).
SourceRange rangeOf(const std::string &Source, const std::string &Fragment,
                    int Line = 1) {
  const auto Pos = static_cast<int>(Source.find(Fragment));
  EXPECT_GE(Pos, 0) << Fragment;
  return {Line, Pos + 1, Line, Pos + 1 + static_cast<int>(Fragment.size())};
}

TEST(Linter, ReferenceHasNoWarnings) {
  EXPECT_TRUE(lintText(readFile("dsl/reference.lab")).empty());
}

TEST(Linter, FunctionArgument) {
  const std::string Source = ruleWhen("sma(close, 20) > close");
  const auto Diagnostics = lintText(Source);
  ASSERT_EQ(Diagnostics.size(), 1u);
  const Diagnostic &D = Diagnostics[0];
  EXPECT_EQ(D.Code, "L001");
  EXPECT_EQ(D.Severity, Level::Warning);
  EXPECT_EQ(D.Range, rangeOf(Source, "20"));
  EXPECT_FALSE(D.Message.empty());
  ASSERT_EQ(D.Fix.size(), 2u);
  // Вставка параметра в начало первой строки первого правила.
  EXPECT_EQ(D.Fix[0].Range, (SourceRange{1, 1, 1, 1}));
  EXPECT_EQ(D.Fix[0].Text, "param sma_n = 20\n");
  // Замена литерала именем.
  EXPECT_EQ(D.Fix[1].Range, rangeOf(Source, "20"));
  EXPECT_EQ(D.Fix[1].Text, "sma_n");
}

TEST(Linter, ComparedWithCall) {
  const auto Diagnostics =
      lintText("param rsi_n = 14\n" + ruleWhen("rsi(close, rsi_n) > 70"));
  ASSERT_EQ(Diagnostics.size(), 1u);
  ASSERT_EQ(Diagnostics[0].Fix.size(), 2u);
  EXPECT_EQ(Diagnostics[0].Fix[0].Text, "param rsi_level = 70\n");
  EXPECT_EQ(Diagnostics[0].Fix[0].Range,
            (SourceRange{2, 1, 2, 1})); // первое правило на второй строке
  EXPECT_EQ(Diagnostics[0].Fix[1].Text, "rsi_level");
}

TEST(Linter, LiteralOnTheLeftOfComparison) {
  const auto Diagnostics = lintText(ruleWhen("70 < rsi(close, 14)"));
  ASSERT_EQ(Diagnostics.size(), 2u);
  EXPECT_EQ(Diagnostics[0].Fix[1].Text, "rsi_level");
  EXPECT_EQ(Diagnostics[1].Fix[1].Text, "rsi_n");
}

TEST(Linter, FallbackNamesAndSuffix) {
  // Литералы вне функций и не рядом с вызовом: p1, p2.
  const auto Plain = lintText(ruleWhen("close * 2 > open * 3"));
  ASSERT_EQ(Plain.size(), 2u);
  EXPECT_EQ(Plain[0].Fix[0].Text, "param p1 = 2\n");
  EXPECT_EQ(Plain[0].Fix[1].Text, "p1");
  EXPECT_EQ(Plain[1].Fix[0].Text, "param p2 = 3\n");
  EXPECT_EQ(Plain[1].Fix[1].Text, "p2");

  // p1 занят параметром: p1_2.
  const auto Taken =
      lintText("param p1 = 5\n" + ruleWhen("close * 2 > open * 3"));
  ASSERT_EQ(Taken.size(), 2u);
  EXPECT_EQ(Taken[0].Fix[1].Text, "p1_2");
  EXPECT_EQ(Taken[1].Fix[1].Text, "p2");
}

TEST(Linter, GeneratedNamesDoNotCollideWithEachOther) {
  const auto Diagnostics =
      lintText(ruleWhen("sma(close, 20) > sma(close, 50)"));
  ASSERT_EQ(Diagnostics.size(), 2u);
  EXPECT_EQ(Diagnostics[0].Fix[1].Text, "sma_n");
  EXPECT_EQ(Diagnostics[1].Fix[1].Text, "sma_n_2");
}

TEST(Linter, AllowsZeroOneHundred) {
  EXPECT_TRUE(
      lintText(
          ruleWhen("position > 0 and close > open * 1 and rsi(close, 1) < 100"))
          .empty());
  EXPECT_TRUE(
      lintText(ruleWhen("close > 0.0 and close < 100.00 and open == 1.0"))
          .empty());
}

TEST(Linter, ActionAndBecauseAreChecked) {
  const std::string Source = "rule \"r\" when position == 0 then buy 95 % cash "
                             "limit close * 0.99 because \"price {close + 7}\"";
  const auto Diagnostics = lintText(Source);
  ASSERT_EQ(Diagnostics.size(), 3u);
  EXPECT_EQ(Diagnostics[0].Range, rangeOf(Source, "95"));
  EXPECT_EQ(Diagnostics[1].Range, rangeOf(Source, "0.99"));
  EXPECT_EQ(Diagnostics[2].Range, rangeOf(Source, "7"));
  EXPECT_EQ(Diagnostics[1].Fix[0].Text, "param p2 = 0.99\n");
}

TEST(Linter, NegativeLiteralKeepsSign) {
  const std::string Source = ruleWhen("close > -5");
  const auto Diagnostics = lintText(Source);
  ASSERT_EQ(Diagnostics.size(), 1u);
  EXPECT_EQ(Diagnostics[0].Range, rangeOf(Source, "5"));
  EXPECT_EQ(Diagnostics[0].Fix[1].Text, "p1");
}

TEST(Linter, ColumnsAreCodePoints) {
  const std::string Source =
      "rule \"правило\" when close > 7 then cancel because \"x\"";
  const auto Diagnostics = lintText(Source);
  ASSERT_EQ(Diagnostics.size(), 1u);
  EXPECT_EQ(Diagnostics[0].Range, (SourceRange{1, 29, 1, 30}));
}

TEST(Linter, FixInsertsBeforeFirstRuleLine) {
  const std::string Source = "# комментарий\nparam a = 3\n\n  rule \"r\" when "
                             "close > 7 then cancel because \"x\"\n";
  const auto Diagnostics = lintText(Source);
  ASSERT_EQ(Diagnostics.size(), 1u);
  EXPECT_EQ(Diagnostics[0].Fix[0].Range, (SourceRange{4, 1, 4, 1}));
  EXPECT_EQ(Diagnostics[0].Range.StartLine, 4);
}

} // namespace
} // namespace core::dsl
