#include "core/dsl/linter.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "core/dsl/parser.hpp"

namespace core::dsl {
namespace {

std::string readFile(const std::string &relativePath) {
  std::ifstream in(std::string(CORE_TESTDATA_DIR) + "/" + relativePath,
                   std::ios::binary);
  EXPECT_TRUE(in.good()) << relativePath;
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::vector<Diagnostic> lintText(const std::string &source) {
  const ParseResult parsed = parse(source);
  EXPECT_TRUE(parsed.program.has_value())
      << (parsed.diagnostics.empty() ? "" : parsed.diagnostics[0].message);
  if (!parsed.program)
    return {};
  return lint(*parsed.program, source);
}

// Правило с одним условием и пустым действием: литералы тестируются в условии.
std::string ruleWhen(const std::string &condition) {
  return "rule \"r\" when " + condition + " then cancel because \"x\"";
}

// Диапазон первого вхождения фрагмента в однострочный текст (столбцы с 1, конец
// исключающий).
SourceRange rangeOf(const std::string &source, const std::string &fragment,
                    int line = 1) {
  const auto pos = static_cast<int>(source.find(fragment));
  EXPECT_GE(pos, 0) << fragment;
  return {line, pos + 1, line, pos + 1 + static_cast<int>(fragment.size())};
}

TEST(Linter, ReferenceHasNoWarnings) {
  EXPECT_TRUE(lintText(readFile("dsl/reference.lab")).empty());
}

TEST(Linter, FunctionArgument) {
  const std::string source = ruleWhen("sma(close, 20) > close");
  const auto diagnostics = lintText(source);
  ASSERT_EQ(diagnostics.size(), 1u);
  const Diagnostic &d = diagnostics[0];
  EXPECT_EQ(d.code, "L001");
  EXPECT_EQ(d.level, Level::Warning);
  EXPECT_EQ(d.range, rangeOf(source, "20"));
  EXPECT_FALSE(d.message.empty());
  ASSERT_EQ(d.fix.size(), 2u);
  // Вставка параметра в начало первой строки первого правила.
  EXPECT_EQ(d.fix[0].range, (SourceRange{1, 1, 1, 1}));
  EXPECT_EQ(d.fix[0].text, "param sma_n = 20\n");
  // Замена литерала именем.
  EXPECT_EQ(d.fix[1].range, rangeOf(source, "20"));
  EXPECT_EQ(d.fix[1].text, "sma_n");
}

TEST(Linter, ComparedWithCall) {
  const auto diagnostics =
      lintText("param rsi_n = 14\n" + ruleWhen("rsi(close, rsi_n) > 70"));
  ASSERT_EQ(diagnostics.size(), 1u);
  ASSERT_EQ(diagnostics[0].fix.size(), 2u);
  EXPECT_EQ(diagnostics[0].fix[0].text, "param rsi_level = 70\n");
  EXPECT_EQ(diagnostics[0].fix[0].range,
            (SourceRange{2, 1, 2, 1})); // первое правило на второй строке
  EXPECT_EQ(diagnostics[0].fix[1].text, "rsi_level");
}

TEST(Linter, LiteralOnTheLeftOfComparison) {
  const auto diagnostics = lintText(ruleWhen("70 < rsi(close, 14)"));
  ASSERT_EQ(diagnostics.size(), 2u);
  EXPECT_EQ(diagnostics[0].fix[1].text, "rsi_level");
  EXPECT_EQ(diagnostics[1].fix[1].text, "rsi_n");
}

TEST(Linter, FallbackNamesAndSuffix) {
  // Литералы вне функций и не рядом с вызовом: p1, p2.
  const auto plain = lintText(ruleWhen("close * 2 > open * 3"));
  ASSERT_EQ(plain.size(), 2u);
  EXPECT_EQ(plain[0].fix[0].text, "param p1 = 2\n");
  EXPECT_EQ(plain[0].fix[1].text, "p1");
  EXPECT_EQ(plain[1].fix[0].text, "param p2 = 3\n");
  EXPECT_EQ(plain[1].fix[1].text, "p2");

  // p1 занят параметром: p1_2.
  const auto taken =
      lintText("param p1 = 5\n" + ruleWhen("close * 2 > open * 3"));
  ASSERT_EQ(taken.size(), 2u);
  EXPECT_EQ(taken[0].fix[1].text, "p1_2");
  EXPECT_EQ(taken[1].fix[1].text, "p2");
}

TEST(Linter, GeneratedNamesDoNotCollideWithEachOther) {
  const auto diagnostics =
      lintText(ruleWhen("sma(close, 20) > sma(close, 50)"));
  ASSERT_EQ(diagnostics.size(), 2u);
  EXPECT_EQ(diagnostics[0].fix[1].text, "sma_n");
  EXPECT_EQ(diagnostics[1].fix[1].text, "sma_n_2");
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
  const std::string source = "rule \"r\" when position == 0 then buy 95 % cash "
                             "limit close * 0.99 because \"price {close + 7}\"";
  const auto diagnostics = lintText(source);
  ASSERT_EQ(diagnostics.size(), 3u);
  EXPECT_EQ(diagnostics[0].range, rangeOf(source, "95"));
  EXPECT_EQ(diagnostics[1].range, rangeOf(source, "0.99"));
  EXPECT_EQ(diagnostics[2].range, rangeOf(source, "7"));
  EXPECT_EQ(diagnostics[1].fix[0].text, "param p2 = 0.99\n");
}

TEST(Linter, NegativeLiteralKeepsSign) {
  const std::string source = ruleWhen("close > -5");
  const auto diagnostics = lintText(source);
  ASSERT_EQ(diagnostics.size(), 1u);
  EXPECT_EQ(diagnostics[0].range, rangeOf(source, "5"));
  EXPECT_EQ(diagnostics[0].fix[1].text, "p1");
}

TEST(Linter, ColumnsAreCodePoints) {
  const std::string source =
      "rule \"правило\" when close > 7 then cancel because \"x\"";
  const auto diagnostics = lintText(source);
  ASSERT_EQ(diagnostics.size(), 1u);
  EXPECT_EQ(diagnostics[0].range, (SourceRange{1, 29, 1, 30}));
}

TEST(Linter, FixInsertsBeforeFirstRuleLine) {
  const std::string source = "# комментарий\nparam a = 3\n\n  rule \"r\" when "
                             "close > 7 then cancel because \"x\"\n";
  const auto diagnostics = lintText(source);
  ASSERT_EQ(diagnostics.size(), 1u);
  EXPECT_EQ(diagnostics[0].fix[0].range, (SourceRange{4, 1, 4, 1}));
  EXPECT_EQ(diagnostics[0].range.startLine, 4);
}

} // namespace
} // namespace core::dsl
