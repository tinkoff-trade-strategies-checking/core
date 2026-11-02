#include "core/dsl/parser.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace core::dsl {
namespace {

std::string readFile(const std::string& relativePath) {
  std::ifstream in(std::string(CORE_TESTDATA_DIR) + "/" + relativePath, std::ios::binary);
  EXPECT_TRUE(in.good()) << relativePath;
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// Выражение в виде строки со скобками вокруг каждого оператора: так видна расстановка приоритетов.
std::string show(const Expr& expr);

std::string showOffset(const std::optional<Offset>& offset) {
  if (!offset) return "";
  std::string text = "[";
  if (offset->negative) text += "-";
  if (const auto* number = std::get_if<std::int64_t>(&offset->value)) {
    text += std::to_string(*number);
  } else {
    text += std::get<std::string>(offset->value);
  }
  return text + "]";
}

std::string show(const Expr& expr) {
  struct Visitor {
    std::string operator()(const Number& n) const { return n.text; }
    std::string operator()(const Bool& b) const { return b.value ? "true" : "false"; }
    std::string operator()(const Na&) const { return "na"; }
    std::string operator()(const Ident& i) const { return i.name + showOffset(i.offset); }
    std::string operator()(const Call& c) const {
      std::string text = c.name + "(";
      for (std::size_t k = 0; k < c.args.size(); ++k) text += (k ? ", " : "") + show(*c.args[k]);
      return text + ")" + showOffset(c.offset);
    }
    std::string operator()(const Unary& u) const {
      return std::string("(") + (u.op == '!' ? "not " : "-") + show(*u.operand) + ")";
    }
    std::string operator()(const Binary& b) const { return "(" + show(*b.left) + " " + b.op + " " + show(*b.right) + ")"; }
  };
  return std::visit(Visitor{}, expr.node);
}

// Правило с условием `when` вокруг подставленного выражения.
std::string whenOf(const std::string& condition) {
  const ParseResult result = parse("rule \"r\" when " + condition + " then cancel because \"x\"");
  EXPECT_TRUE(result.program.has_value()) << (result.diagnostics.empty() ? "" : result.diagnostics[0].message);
  if (!result.program) return "<ошибка разбора>";
  return show(*result.program->rules[0].when);
}

// Первая (и единственная) диагностика разбора, который обязан упасть.
Diagnostic syntaxError(const std::string& source) {
  const ParseResult result = parse(source);
  EXPECT_FALSE(result.program.has_value()) << source;
  EXPECT_EQ(result.diagnostics.size(), 1U) << source;
  if (result.diagnostics.empty()) return {};
  EXPECT_EQ(result.diagnostics[0].code, "E-SYN");
  EXPECT_EQ(result.diagnostics[0].level, Level::Error);
  return result.diagnostics[0];
}

Action actionOf(const std::string& action) {
  ParseResult result = parse("rule \"r\" when true then " + action + " because \"x\"");
  EXPECT_TRUE(result.program.has_value()) << action << ": " << (result.diagnostics.empty() ? "" : result.diagnostics[0].message);
  if (!result.program) return {};
  return std::move(result.program->rules[0].action);
}

}  // namespace

TEST(Parser, ParsesReferenceStrategy) {
  const ParseResult result = parse(readFile("dsl/reference.lab"));
  ASSERT_TRUE(result.program.has_value());
  EXPECT_TRUE(result.diagnostics.empty());
  const Program& program = *result.program;
  ASSERT_EQ(program.params.size(), 6U);
  ASSERT_EQ(program.rules.size(), 4U);

  EXPECT_EQ(program.params[0].name, "fast");
  EXPECT_EQ(program.params[0].value, 20);
  EXPECT_EQ(program.params[0].min, 5);
  EXPECT_EQ(program.params[0].max, 50);
  EXPECT_EQ(program.params[0].step, 5);
  EXPECT_EQ(program.params[2].name, "rsi_n");
  EXPECT_FALSE(program.params[2].min.has_value());
  EXPECT_FALSE(program.params[5].step.has_value());

  EXPECT_EQ(program.rules[0].name, "вход");
  EXPECT_EQ(program.rules[1].name, "стоп");
  EXPECT_EQ(program.rules[1].action.kind, Action::Kind::Sell);
  EXPECT_EQ(program.rules[1].action.sizeKind, SizeKind::All);
  EXPECT_EQ(program.rules[1].action.orderKind, OrderKind::Stop);
  ASSERT_NE(program.rules[1].action.price, nullptr);
  EXPECT_EQ(show(*program.rules[1].action.price), "(entry_price * (1 - (stop_pct / 100)))");
  EXPECT_EQ(program.rules[0].action.sizeKind, SizeKind::PercentCash);
  EXPECT_EQ(show(*program.rules[0].when),
            "(crossover(sma(close, fast), sma(close, slow)) and (position == 0))");
  EXPECT_EQ(program.rules[3].action.orderKind, OrderKind::Market);
}

TEST(Parser, NegativeOffsetIsSyntacticallyValid) {
  const ParseResult result = parse(R"(rule "r" when close[-1] > close then buy 10 lots because "x")");
  ASSERT_TRUE(result.program.has_value());
  const auto& binary = std::get<Binary>(result.program->rules[0].when->node);
  const auto& ident = std::get<Ident>(binary.left->node);
  ASSERT_TRUE(ident.offset.has_value());
  EXPECT_TRUE(ident.offset->negative);
  EXPECT_EQ(std::get<std::int64_t>(ident.offset->value), 1);
  // Диапазон смещения - вместе со скобками: `[-1]` в столбцах 20-23.
  EXPECT_EQ(ident.offset->range, (SourceRange{1, 20, 1, 24}));
}

TEST(Parser, MissingThen) {
  const Diagnostic d = syntaxError(R"(rule "a" when close > open buy 1 lots because "x")");
  EXPECT_EQ(d.message, "ожидалось `then`");
  EXPECT_EQ(d.range, (SourceRange{1, 28, 1, 31}));
}

TEST(Parser, StringInterpolation) {
  const ParseResult result = parse(R"(rule "r" when true then cancel because "RSI = {rsi(close, 14)}, {{x}}")");
  ASSERT_TRUE(result.program.has_value());
  const auto& parts = result.program->rules[0].because.parts;
  ASSERT_EQ(parts.size(), 3U);
  EXPECT_EQ(std::get<std::string>(parts[0]), "RSI = ");
  EXPECT_EQ(show(*std::get<ExprPtr>(parts[1])), "rsi(close, 14)");
  EXPECT_EQ(std::get<std::string>(parts[2]), ", {x}");
}

TEST(Parser, Precedence) {
  EXPECT_EQ(whenOf("a or b and not c < 1 + 2 * 3"), "(a or (b and (not (c < (1 + (2 * 3))))))");
  EXPECT_EQ(whenOf("a - b - c"), "((a - b) - c)");
  EXPECT_EQ(whenOf("a / b * c"), "((a / b) * c)");
  EXPECT_EQ(whenOf("(a or b) and c"), "((a or b) and c)");
  EXPECT_EQ(whenOf("-a * b"), "((-a) * b)");
}

// ---- лексика ----

TEST(Parser, ColumnsCountCodePointsNotBytes) {
  const Diagnostic d = syntaxError(R"(rule "вход" when close > open buy 1 lots because "x")");
  EXPECT_EQ(d.message, "ожидалось `then`");
  EXPECT_EQ(d.range, (SourceRange{1, 31, 1, 34}));
}

TEST(Parser, ErrorPositionOnLaterLine) {
  const Diagnostic d = syntaxError("# комментарий\nrule \"a\"\n  when close > open\n  buy 1 lots because \"x\"");
  EXPECT_EQ(d.message, "ожидалось `then`");
  EXPECT_EQ(d.range, (SourceRange{4, 3, 4, 6}));
}

TEST(Parser, CommentsAndCrLfAreTrivia) {
  const ParseResult result = parse("param a = 1 # один\r\nrule \"r\" # имя\r\n when true then cancel because \"x\"\r\n");
  ASSERT_TRUE(result.program.has_value());
  EXPECT_EQ(result.program->params.size(), 1U);
}

TEST(Parser, NumbersAndRangeOperatorAreLexedApart) {
  const ParseResult result = parse("param a = -2.5 range -10..10.5 step 0.5\nrule \"r\" when 1.50 > 0 then cancel because \"x\"");
  ASSERT_TRUE(result.program.has_value());
  const ParamDecl& a = result.program->params[0];
  EXPECT_EQ(a.value, -2.5);
  EXPECT_EQ(a.min, -10);
  EXPECT_EQ(a.max, 10.5);
  EXPECT_EQ(a.step, 0.5);
  const auto& binary = std::get<Binary>(result.program->rules[0].when->node);
  EXPECT_EQ(std::get<Number>(binary.left->node).text, "1.50");  // литерал сохраняется как написан
  EXPECT_EQ(std::get<Number>(binary.left->node).value, 1.5);
}

TEST(Parser, StringEscapesAndBraces) {
  const ParseResult result = parse(R"(rule "a \"b\" \\ {{c}}" when true then cancel because "")");
  ASSERT_TRUE(result.program.has_value());
  EXPECT_EQ(result.program->rules[0].name, "a \"b\" \\ {c}");
  EXPECT_TRUE(result.program->rules[0].because.parts.empty());
}

TEST(Parser, PortfolioNamesAreExpressionsDespiteBeingKeywords) {
  EXPECT_EQ(whenOf("position > 0 and cash > equity"), "((position > 0) and (cash > equity))");
}

TEST(Parser, OffsetsOnSeriesAndCalls) {
  EXPECT_EQ(whenOf("close[2] > sma(close, 5)[k] and open[-k] > 0"),
            "((close[2] > sma(close, 5)[k]) and (open[-k] > 0))");
}

// ---- действия ----

TEST(Parser, ActionSizes) {
  Action a = actionOf("buy 10 lots");
  EXPECT_EQ(a.kind, Action::Kind::Buy);
  EXPECT_EQ(a.sizeKind, SizeKind::Lots);
  EXPECT_EQ(show(*a.size), "10");
  EXPECT_EQ(a.orderKind, OrderKind::Market);
  EXPECT_EQ(a.price, nullptr);
  EXPECT_FALSE(a.day.has_value());

  EXPECT_EQ(actionOf("buy 50 % equity").sizeKind, SizeKind::PercentEquity);
  EXPECT_EQ(actionOf("buy size % cash").sizeKind, SizeKind::PercentCash);
  a = actionOf("sell 50 % position");
  EXPECT_EQ(a.kind, Action::Kind::Sell);
  EXPECT_EQ(a.sizeKind, SizeKind::PercentPosition);
  a = actionOf("sell all");
  EXPECT_EQ(a.sizeKind, SizeKind::All);
  EXPECT_EQ(a.size, nullptr);
  EXPECT_EQ(actionOf("cancel").kind, Action::Kind::Cancel);
  // `buy all` - ошибка типа, а не синтаксиса: дерево строится.
  EXPECT_EQ(actionOf("buy all").sizeKind, SizeKind::All);
}

TEST(Parser, ActionOrdersAndTimeInForce) {
  Action a = actionOf("buy 1 lots limit close * 0.99 day");
  EXPECT_EQ(a.orderKind, OrderKind::Limit);
  EXPECT_EQ(show(*a.price), "(close * 0.99)");
  EXPECT_EQ(a.day, std::optional<bool>(true));

  a = actionOf("sell all take_profit entry_price * 1.1 gtc");
  EXPECT_EQ(a.orderKind, OrderKind::TakeProfit);
  EXPECT_EQ(a.day, std::optional<bool>(false));

  a = actionOf("buy 1 lots market day");
  EXPECT_EQ(a.orderKind, OrderKind::Market);
  EXPECT_EQ(a.price, nullptr);
  EXPECT_EQ(a.day, std::optional<bool>(true));

  EXPECT_EQ(actionOf("sell 1 lots stop low").orderKind, OrderKind::Stop);
}

// ---- ошибки ----

TEST(Parser, ExpectedTokenMessages) {
  EXPECT_EQ(syntaxError("").message, "ожидалось `param` или `rule`");
  EXPECT_EQ(syntaxError("param").message, "ожидалось имя параметра");
  EXPECT_EQ(syntaxError("param a 5").message, "ожидалось `=`");
  EXPECT_EQ(syntaxError("param a = x").message, "ожидалось число");
  EXPECT_EQ(syntaxError("param a = 5 range 1 5 step 1").message, "ожидалось `..`");
  EXPECT_EQ(syntaxError("param a = 5 range 1..9").message, "ожидалось `step`");
  EXPECT_EQ(syntaxError(R"(rule "a" close)").message, "ожидалось `when`");
  EXPECT_EQ(syntaxError(R"(rule "a" when then)").message, "ожидалось выражение");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then hold)").message, "ожидалось `buy`, `sell` или `cancel`");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then buy 1)").message, "ожидалось `lots` или `%`");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then buy 1 % lots)").message, "ожидалось `equity`, `cash` или `position`");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then cancel)").message, "ожидалось `because`");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then cancel because 5)").message, "ожидалась строка с причиной");
  EXPECT_EQ(syntaxError(R"(rule "a" when (1 + 2 then cancel because "x")").message, "ожидалось `)`");
  EXPECT_EQ(syntaxError(R"(rule "a" when f(1 2) then cancel because "x")").message, "ожидалось `,` или `)`");
  EXPECT_EQ(syntaxError(R"(rule "a" when close[1.5] then cancel because "x")").message,
            "ожидалось целое число или имя параметра");
  EXPECT_EQ(syntaxError(R"(rule "a" when close[1 then cancel because "x")").message, "ожидалось `]`");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then cancel because "x" param)").message, "ожидалось `rule` или конец текста");
}

TEST(Parser, GrammarIsExactlyTheEbnf) {
  // not и унарный минус - не больше одного, сравнение не цепляется, ключевые слова не имена.
  syntaxError(R"(rule "a" when not not true then cancel because "x")");
  syntaxError(R"(rule "a" when - - 1 > 0 then cancel because "x")");
  syntaxError(R"(rule "a" when 1 < 2 < 3 then cancel because "x")");
  syntaxError(R"(param lots = 1 rule "a" when true then cancel because "x")");
  syntaxError(R"(rule "a" when (close)[1] > 0 then cancel because "x")");
  syntaxError(R"(rule "a" when close[1][2] > 0 then cancel because "x")");
  syntaxError(R"(param a = 1 param b = 2)");  // правил нет
  syntaxError(R"(rule "a" when true then cancel because "x" param a = 1)");  // параметры только перед правилами
}

TEST(Parser, LexicalErrors) {
  EXPECT_EQ(syntaxError("rule \"a\" when Close > 0 then cancel because \"x\"").range, (SourceRange{1, 15, 1, 16}));
  EXPECT_EQ(syntaxError("rule \"a\" when a ! b").message,
            "недопустимый символ `!`: отрицание пишется словом `not`");
  EXPECT_EQ(syntaxError("rule \"a\" when a & b").message, "недопустимый символ `&`");
  EXPECT_EQ(syntaxError("rule \"a\" when я > 0").message, "недопустимый символ `я`");
  EXPECT_EQ(syntaxError("rule \"a\" when a . b").message, "недопустимый символ `.`");
  EXPECT_EQ(syntaxError("rule \"a\" when a { b").message, "недопустимый символ `{`");
  EXPECT_EQ(syntaxError("rule \"a").range, (SourceRange{1, 6, 1, 7}));
  EXPECT_EQ(syntaxError("rule \"a").message, "строка не закрыта, ожидалась `\"`");
  EXPECT_EQ(syntaxError(R"(rule "a\n" when)").message, "недопустимая escape-последовательность, допустимы `\\\"` и `\\\\`");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then cancel because "}")").message, "одиночная `}` в строке: скобка записывается как `}}`");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then cancel because "{1 + 2")").message, "подстановка не закрыта, ожидалась `}`");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then cancel because "{}")").message, "ожидалось выражение");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then cancel because "{1 2}")").message, "ожидалось `}`");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then cancel because "{"x"}")").message, "подстановка не закрыта, ожидалась `}`");
  EXPECT_EQ(syntaxError(R"(rule "{x}" when true then cancel because "y")").message, "в названии правила подстановки не допускаются");
  EXPECT_EQ(syntaxError(R"(rule "a" when close[99999999999999999999] > 0 then cancel because "x")").message, "число вне диапазона");
}

TEST(Parser, InvalidUtf8) {
  const Diagnostic d = syntaxError(std::string("# ") + "\xC3\x28" + "\nrule");
  EXPECT_EQ(d.message, "недопустимая последовательность байтов UTF-8");
  EXPECT_EQ(d.range, (SourceRange{1, 3, 1, 4}));
}

TEST(Parser, StopsAtFirstError) {
  const ParseResult result = parse(R"(rule "a" when ) then ? because "x" rule)");
  ASSERT_EQ(result.diagnostics.size(), 1U);
  EXPECT_EQ(result.diagnostics[0].range.startColumn, 15);
}

// ---- ограничения ----

TEST(Parser, LimitOnParamsAndRules) {
  std::string params;
  for (int k = 0; k < 50; ++k) params += "param p" + std::string(1, static_cast<char>('a' + k / 26)) + std::string(1, static_cast<char>('a' + k % 26)) + " = 1\n";
  const std::string rule = "rule \"r\" when true then cancel because \"x\"\n";
  EXPECT_TRUE(parse(params + rule).program.has_value());
  const Diagnostic tooManyParams = syntaxError(params + "param extra = 1\n" + rule);
  EXPECT_EQ(tooManyParams.message, "слишком много параметров: не более 50");
  EXPECT_EQ(tooManyParams.range.startLine, 51);

  std::string rules;
  for (int k = 0; k < 100; ++k) rules += rule;
  EXPECT_TRUE(parse(rules).program.has_value());
  const Diagnostic tooManyRules = syntaxError(rules + rule);
  EXPECT_EQ(tooManyRules.message, "слишком много правил: не более 100");
  EXPECT_EQ(tooManyRules.range.startLine, 101);
}

TEST(Parser, LimitOnSourceSize) {
  const std::string rule = "rule \"r\" when true then cancel because \"x\"\n";
  std::string source = rule;
  source += "#" + std::string(64 * 1024 - source.size() - 1, 'x');
  ASSERT_EQ(source.size(), 64U * 1024U);
  EXPECT_TRUE(parse(source).program.has_value());
  const Diagnostic tooLong = syntaxError(source + "x");
  EXPECT_EQ(tooLong.message, "текст длиннее 64 КиБ");
  EXPECT_EQ(tooLong.range, (SourceRange{1, 1, 1, 2}));
}

TEST(Parser, DeepNestingIsRejectedNotOverflowed) {
  const std::string prefix = "rule \"r\" when ";
  const std::string suffix = " then cancel because \"x\"";
  EXPECT_TRUE(parse(prefix + std::string(40, '(') + "true" + std::string(40, ')') + suffix).program.has_value());
  EXPECT_EQ(syntaxError(prefix + std::string(20000, '(') + "true" + std::string(20000, ')') + suffix).message,
            "слишком глубокая вложенность выражения: не более 100 уровней");
  std::string chain = "1";
  for (int k = 0; k < 20000; ++k) chain += "+1";
  EXPECT_EQ(syntaxError(prefix + chain + suffix).message, "слишком глубокая вложенность выражения: не более 100 уровней");
}

TEST(Parser, TreeHeightIsBoundedThroughNestedParens) {
  // 99 уровней скобок, после каждой `)` цепочка `+1`: источник мал, а дерево было бы в тысячи уровней.
  std::string expr = "1";
  for (int level = 1; level <= 99; ++level) {
    expr = "(" + expr + ")";
    for (int k = 0; k < 100 - level; ++k) expr += "+1";
  }
  const std::string source = "rule \"r\" when " + expr + " > 0 then cancel because \"x\"";
  ASSERT_LT(source.size(), 64U * 1024U);
  EXPECT_EQ(syntaxError(source).message, "слишком глубокая вложенность выражения: не более 100 уровней");

  // Высота считается по узлам: цепочка из 50 слагаемых, обёрнутая в скобки, проходит.
  std::string chain = "a";
  for (int k = 0; k < 49; ++k) chain += "+a";
  const ParseResult ok = parse("rule \"r\" when ((" + chain + ")) > 0 then cancel because \"x\"");
  ASSERT_TRUE(ok.program.has_value());
  EXPECT_EQ(ok.program->rules[0].when->height, 51);
}

TEST(Parser, HashInSubstitutionIsNotAComment) {
  // В тексте строки `#` обычный символ; в подстановке - недопустимый символ, а не комментарий до конца строки.
  const ParseResult text = parse(R"(rule "r" when true then cancel because "a # b {1}")");
  ASSERT_TRUE(text.program.has_value());
  EXPECT_EQ(std::get<std::string>(text.program->rules[0].because.parts[0]), "a # b ");
  EXPECT_EQ(syntaxError("rule \"r\" when true then cancel because \"{1 # }\"").message, "недопустимый символ `#`");
}

TEST(Parser, CarriageReturnsAreStrippedFromStrings) {
  const ParseResult result = parse("rule \"a\r\nb\" when true then cancel because \"x\r\n{1}\"\r\n");
  ASSERT_TRUE(result.program.has_value());
  EXPECT_EQ(result.program->rules[0].name, "a\nb");
  EXPECT_EQ(std::get<std::string>(result.program->rules[0].because.parts[0]), "x\n");
}

// ---- диапазоны ----

TEST(Parser, NodeRanges) {
  const ParseResult result = parse("param a = 1\nrule \"r\" when (a + 2) * 3 > 4\n  then buy 5 lots limit close\n  because \"x\"");
  ASSERT_TRUE(result.program.has_value());
  const Program& p = *result.program;
  EXPECT_EQ(p.params[0].range, (SourceRange{1, 1, 1, 12}));
  const Rule& rule = p.rules[0];
  EXPECT_EQ(rule.range, (SourceRange{2, 1, 4, 14}));
  EXPECT_EQ(rule.when->range, (SourceRange{2, 15, 2, 30}));
  const auto& cmp = std::get<Binary>(rule.when->node);
  const auto& product = std::get<Binary>(cmp.left->node);
  EXPECT_EQ(cmp.left->range, (SourceRange{2, 15, 2, 26}));
  EXPECT_EQ(product.left->range, (SourceRange{2, 15, 2, 22}));  // со скобками
  EXPECT_EQ(std::get<Binary>(product.left->node).right->range, (SourceRange{2, 20, 2, 21}));  // литерал 2
  EXPECT_EQ(rule.action.range, (SourceRange{3, 8, 3, 30}));
  EXPECT_EQ(rule.because.range, (SourceRange{4, 11, 4, 14}));
}

}  // namespace core::dsl
