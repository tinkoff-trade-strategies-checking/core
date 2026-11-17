#include "core/dsl/parser.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

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

// Выражение в виде строки со скобками вокруг каждого оператора: так видна
// расстановка приоритетов.
std::string show(const Expr &Expr);

std::string showOffset(const std::optional<Offset> &Offset) {
  if (!Offset)
    return "";
  std::string Text = "[";
  if (Offset->Negative)
    Text += "-";
  if (const auto *Number = std::get_if<std::int64_t>(&Offset->Value)) {
    Text += std::to_string(*Number);
  } else {
    Text += std::get<std::string>(Offset->Value);
  }
  return Text + "]";
}

std::string show(const Expr &Expr) {
  struct Visitor {
    std::string operator()(const Number &N) const { return N.Text; }
    std::string operator()(const Bool &B) const {
      return B.Value ? "true" : "false";
    }
    std::string operator()(const Na &) const { return "na"; }
    std::string operator()(const Ident &I) const {
      return I.Name + showOffset(I.BarOffset);
    }
    std::string operator()(const Call &C) const {
      std::string Text = C.Name + "(";
      for (std::size_t K = 0; K < C.Args.size(); ++K)
        Text += (K ? ", " : "") + show(*C.Args[K]);
      return Text + ")" + showOffset(C.BarOffset);
    }
    std::string operator()(const Unary &U) const {
      return std::string("(") + (U.Op == '!' ? "not " : "-") +
             show(*U.Operand) + ")";
    }
    std::string operator()(const Binary &B) const {
      return "(" + show(*B.Left) + " " + B.Op + " " + show(*B.Right) + ")";
    }
  };
  return std::visit(Visitor{}, Expr.Node);
}

// Правило с условием `when` вокруг подставленного выражения.
std::string whenOf(const std::string &Condition) {
  const ParseResult Result =
      parse("rule \"r\" when " + Condition + " then cancel because \"x\"");
  EXPECT_TRUE(Result.Ast.has_value())
      << (Result.Diagnostics.empty() ? "" : Result.Diagnostics[0].Message);
  if (!Result.Ast)
    return "<ошибка разбора>";
  return show(*Result.Ast->Rules[0].When);
}

// Первая (и единственная) диагностика разбора, который обязан упасть.
Diagnostic syntaxError(const std::string &Source) {
  const ParseResult Result = parse(Source);
  EXPECT_FALSE(Result.Ast.has_value()) << Source;
  EXPECT_EQ(Result.Diagnostics.size(), 1U) << Source;
  if (Result.Diagnostics.empty())
    return {};
  EXPECT_EQ(Result.Diagnostics[0].Code, "E-SYN");
  EXPECT_EQ(Result.Diagnostics[0].Severity, Level::Error);
  return Result.Diagnostics[0];
}

Action actionOf(const std::string &Action) {
  ParseResult Result =
      parse("rule \"r\" when true then " + Action + " because \"x\"");
  EXPECT_TRUE(Result.Ast.has_value())
      << Action << ": "
      << (Result.Diagnostics.empty() ? "" : Result.Diagnostics[0].Message);
  if (!Result.Ast)
    return {};
  return std::move(Result.Ast->Rules[0].Then);
}

} // namespace

TEST(Parser, ParsesReferenceStrategy) {
  const ParseResult Result = parse(readFile("dsl/reference.lab"));
  ASSERT_TRUE(Result.Ast.has_value());
  EXPECT_TRUE(Result.Diagnostics.empty());
  const Program &Program = *Result.Ast;
  ASSERT_EQ(Program.Params.size(), 6U);
  ASSERT_EQ(Program.Rules.size(), 4U);

  EXPECT_EQ(Program.Params[0].Name, "fast");
  EXPECT_EQ(Program.Params[0].Value, 20);
  EXPECT_EQ(Program.Params[0].Min, 5);
  EXPECT_EQ(Program.Params[0].Max, 50);
  EXPECT_EQ(Program.Params[0].Step, 5);
  EXPECT_EQ(Program.Params[2].Name, "rsi_n");
  EXPECT_FALSE(Program.Params[2].Min.has_value());
  EXPECT_FALSE(Program.Params[5].Step.has_value());

  EXPECT_EQ(Program.Rules[0].Name, "вход");
  EXPECT_EQ(Program.Rules[1].Name, "стоп");
  EXPECT_EQ(Program.Rules[1].Then.ActionKind, Action::Kind::Sell);
  EXPECT_EQ(Program.Rules[1].Then.SizeUnit, SizeKind::All);
  EXPECT_EQ(Program.Rules[1].Then.OrderMode, OrderKind::Stop);
  ASSERT_NE(Program.Rules[1].Then.Price, nullptr);
  EXPECT_EQ(show(*Program.Rules[1].Then.Price),
            "(entry_price * (1 - (stop_pct / 100)))");
  EXPECT_EQ(Program.Rules[0].Then.SizeUnit, SizeKind::PercentCash);
  EXPECT_EQ(
      show(*Program.Rules[0].When),
      "(crossover(sma(close, fast), sma(close, slow)) and (position == 0))");
  EXPECT_EQ(Program.Rules[3].Then.OrderMode, OrderKind::Market);
}

TEST(Parser, NegativeOffsetIsSyntacticallyValid) {
  const ParseResult Result =
      parse(R"(rule "r" when close[-1] > close then buy 10 lots because "x")");
  ASSERT_TRUE(Result.Ast.has_value());
  const auto &BinaryNode = std::get<Binary>(Result.Ast->Rules[0].When->Node);
  const auto &IdentNode = std::get<Ident>(BinaryNode.Left->Node);
  ASSERT_TRUE(IdentNode.BarOffset.has_value());
  EXPECT_TRUE(IdentNode.BarOffset->Negative);
  EXPECT_EQ(std::get<std::int64_t>(IdentNode.BarOffset->Value), 1);
  // Диапазон смещения - вместе со скобками: `[-1]` в столбцах 20-23.
  EXPECT_EQ(IdentNode.BarOffset->Range, (SourceRange{1, 20, 1, 24}));
}

TEST(Parser, MissingThen) {
  const Diagnostic D =
      syntaxError(R"(rule "a" when close > open buy 1 lots because "x")");
  EXPECT_EQ(D.Message, "ожидалось `then`");
  EXPECT_EQ(D.Range, (SourceRange{1, 28, 1, 31}));
}

TEST(Parser, StringInterpolation) {
  const ParseResult Result = parse(
      R"(rule "r" when true then cancel because "RSI = {rsi(close, 14)}, {{x}}")");
  ASSERT_TRUE(Result.Ast.has_value());
  const auto &Parts = Result.Ast->Rules[0].Because.Parts;
  ASSERT_EQ(Parts.size(), 3U);
  EXPECT_EQ(std::get<std::string>(Parts[0]), "RSI = ");
  EXPECT_EQ(show(*std::get<ExprPtr>(Parts[1])), "rsi(close, 14)");
  EXPECT_EQ(std::get<std::string>(Parts[2]), ", {x}");
}

TEST(Parser, Precedence) {
  EXPECT_EQ(whenOf("a or b and not c < 1 + 2 * 3"),
            "(a or (b and (not (c < (1 + (2 * 3))))))");
  EXPECT_EQ(whenOf("a - b - c"), "((a - b) - c)");
  EXPECT_EQ(whenOf("a / b * c"), "((a / b) * c)");
  EXPECT_EQ(whenOf("(a or b) and c"), "((a or b) and c)");
  EXPECT_EQ(whenOf("-a * b"), "((-a) * b)");
}

// ---- лексика ----

TEST(Parser, ColumnsCountCodePointsNotBytes) {
  const Diagnostic D =
      syntaxError(R"(rule "вход" when close > open buy 1 lots because "x")");
  EXPECT_EQ(D.Message, "ожидалось `then`");
  EXPECT_EQ(D.Range, (SourceRange{1, 31, 1, 34}));
}

TEST(Parser, ErrorPositionOnLaterLine) {
  const Diagnostic D = syntaxError("# комментарий\nrule \"a\"\n  when close > "
                                   "open\n  buy 1 lots because \"x\"");
  EXPECT_EQ(D.Message, "ожидалось `then`");
  EXPECT_EQ(D.Range, (SourceRange{4, 3, 4, 6}));
}

TEST(Parser, CommentsAndCrLfAreTrivia) {
  const ParseResult Result = parse("param a = 1 # один\r\nrule \"r\" # имя\r\n "
                                   "when true then cancel because \"x\"\r\n");
  ASSERT_TRUE(Result.Ast.has_value());
  EXPECT_EQ(Result.Ast->Params.size(), 1U);
}

TEST(Parser, NumbersAndRangeOperatorAreLexedApart) {
  const ParseResult Result =
      parse("param a = -2.5 range -10..10.5 step 0.5\nrule \"r\" when 1.50 > 0 "
            "then cancel because \"x\"");
  ASSERT_TRUE(Result.Ast.has_value());
  const ParamDecl &A = Result.Ast->Params[0];
  EXPECT_EQ(A.Value, -2.5);
  EXPECT_EQ(A.Min, -10);
  EXPECT_EQ(A.Max, 10.5);
  EXPECT_EQ(A.Step, 0.5);
  const auto &Bin = std::get<Binary>(Result.Ast->Rules[0].When->Node);
  EXPECT_EQ(std::get<Number>(Bin.Left->Node).Text,
            "1.50"); // литерал сохраняется как написан
  EXPECT_EQ(std::get<Number>(Bin.Left->Node).Value, 1.5);
}

TEST(Parser, StringEscapesAndBraces) {
  const ParseResult Result =
      parse(R"(rule "a \"b\" \\ {{c}}" when true then cancel because "")");
  ASSERT_TRUE(Result.Ast.has_value());
  EXPECT_EQ(Result.Ast->Rules[0].Name, "a \"b\" \\ {c}");
  EXPECT_TRUE(Result.Ast->Rules[0].Because.Parts.empty());
}

TEST(Parser, PortfolioNamesAreExpressionsDespiteBeingKeywords) {
  EXPECT_EQ(whenOf("position > 0 and cash > equity"),
            "((position > 0) and (cash > equity))");
}

TEST(Parser, OffsetsOnSeriesAndCalls) {
  EXPECT_EQ(whenOf("close[2] > sma(close, 5)[k] and open[-k] > 0"),
            "((close[2] > sma(close, 5)[k]) and (open[-k] > 0))");
}

// ---- действия ----

TEST(Parser, ActionSizes) {
  Action A = actionOf("buy 10 lots");
  EXPECT_EQ(A.ActionKind, Action::Kind::Buy);
  EXPECT_EQ(A.SizeUnit, SizeKind::Lots);
  EXPECT_EQ(show(*A.Size), "10");
  EXPECT_EQ(A.OrderMode, OrderKind::Market);
  EXPECT_EQ(A.Price, nullptr);
  EXPECT_FALSE(A.Day.has_value());

  EXPECT_EQ(actionOf("buy 50 % equity").SizeUnit, SizeKind::PercentEquity);
  EXPECT_EQ(actionOf("buy size % cash").SizeUnit, SizeKind::PercentCash);
  A = actionOf("sell 50 % position");
  EXPECT_EQ(A.ActionKind, Action::Kind::Sell);
  EXPECT_EQ(A.SizeUnit, SizeKind::PercentPosition);
  A = actionOf("sell all");
  EXPECT_EQ(A.SizeUnit, SizeKind::All);
  EXPECT_EQ(A.Size, nullptr);
  EXPECT_EQ(actionOf("cancel").ActionKind, Action::Kind::Cancel);
  // `buy all` - ошибка типа, а не синтаксиса: дерево строится.
  EXPECT_EQ(actionOf("buy all").SizeUnit, SizeKind::All);
}

TEST(Parser, ActionOrdersAndTimeInForce) {
  Action A = actionOf("buy 1 lots limit close * 0.99 day");
  EXPECT_EQ(A.OrderMode, OrderKind::Limit);
  EXPECT_EQ(show(*A.Price), "(close * 0.99)");
  EXPECT_EQ(A.Day, std::optional<bool>(true));

  A = actionOf("sell all take_profit entry_price * 1.1 gtc");
  EXPECT_EQ(A.OrderMode, OrderKind::TakeProfit);
  EXPECT_EQ(A.Day, std::optional<bool>(false));

  A = actionOf("buy 1 lots market day");
  EXPECT_EQ(A.OrderMode, OrderKind::Market);
  EXPECT_EQ(A.Price, nullptr);
  EXPECT_EQ(A.Day, std::optional<bool>(true));

  EXPECT_EQ(actionOf("sell 1 lots stop low").OrderMode, OrderKind::Stop);
}

// ---- ошибки ----

TEST(Parser, ExpectedTokenMessages) {
  EXPECT_EQ(syntaxError("").Message, "ожидалось `param` или `rule`");
  EXPECT_EQ(syntaxError("param").Message, "ожидалось имя параметра");
  EXPECT_EQ(syntaxError("param a 5").Message, "ожидалось `=`");
  EXPECT_EQ(syntaxError("param a = x").Message, "ожидалось число");
  EXPECT_EQ(syntaxError("param a = 5 range 1 5 step 1").Message,
            "ожидалось `..`");
  EXPECT_EQ(syntaxError("param a = 5 range 1..9").Message, "ожидалось `step`");
  EXPECT_EQ(syntaxError(R"(rule "a" close)").Message, "ожидалось `when`");
  EXPECT_EQ(syntaxError(R"(rule "a" when then)").Message,
            "ожидалось выражение");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then hold)").Message,
            "ожидалось `buy`, `sell` или `cancel`");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then buy 1)").Message,
            "ожидалось `lots` или `%`");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then buy 1 % lots)").Message,
            "ожидалось `equity`, `cash` или `position`");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then cancel)").Message,
            "ожидалось `because`");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then cancel because 5)").Message,
            "ожидалась строка с причиной");
  EXPECT_EQ(
      syntaxError(R"(rule "a" when (1 + 2 then cancel because "x")").Message,
      "ожидалось `)`");
  EXPECT_EQ(
      syntaxError(R"(rule "a" when f(1 2) then cancel because "x")").Message,
      "ожидалось `,` или `)`");
  EXPECT_EQ(syntaxError(R"(rule "a" when close[1.5] then cancel because "x")")
                .Message,
            "ожидалось целое число или имя параметра");
  EXPECT_EQ(
      syntaxError(R"(rule "a" when close[1 then cancel because "x")").Message,
      "ожидалось `]`");
  EXPECT_EQ(syntaxError(R"(rule "a" when true then cancel because "x" param)")
                .Message,
            "ожидалось `rule` или конец текста");
}

TEST(Parser, GrammarIsExactlyTheEbnf) {
  // not и унарный минус - не больше одного, сравнение не цепляется, ключевые
  // слова не имена.
  syntaxError(R"(rule "a" when not not true then cancel because "x")");
  syntaxError(R"(rule "a" when - - 1 > 0 then cancel because "x")");
  syntaxError(R"(rule "a" when 1 < 2 < 3 then cancel because "x")");
  syntaxError(R"(param lots = 1 rule "a" when true then cancel because "x")");
  syntaxError(R"(rule "a" when (close)[1] > 0 then cancel because "x")");
  syntaxError(R"(rule "a" when close[1][2] > 0 then cancel because "x")");
  syntaxError(R"(param a = 1 param b = 2)"); // правил нет
  syntaxError(
      R"(rule "a" when true then cancel because "x" param a = 1)"); // параметры
                                                                    // только
                                                                    // перед
                                                                    // правилами
}

TEST(Parser, LexicalErrors) {
  EXPECT_EQ(
      syntaxError("rule \"a\" when Close > 0 then cancel because \"x\"").Range,
      (SourceRange{1, 15, 1, 16}));
  EXPECT_EQ(syntaxError("rule \"a\" when a ! b").Message,
            "недопустимый символ `!`: отрицание пишется словом `not`");
  EXPECT_EQ(syntaxError("rule \"a\" when a & b").Message,
            "недопустимый символ `&`");
  EXPECT_EQ(syntaxError("rule \"a\" when я > 0").Message,
            "недопустимый символ `я`");
  EXPECT_EQ(syntaxError("rule \"a\" when a . b").Message,
            "недопустимый символ `.`");
  EXPECT_EQ(syntaxError("rule \"a\" when a { b").Message,
            "недопустимый символ `{`");
  EXPECT_EQ(syntaxError("rule \"a").Range, (SourceRange{1, 6, 1, 7}));
  EXPECT_EQ(syntaxError("rule \"a").Message,
            "строка не закрыта, ожидалась `\"`");
  EXPECT_EQ(
      syntaxError(R"(rule "a\n" when)").Message,
      "недопустимая escape-последовательность, допустимы `\\\"` и `\\\\`");
  EXPECT_EQ(
      syntaxError(R"(rule "a" when true then cancel because "}")").Message,
      "одиночная `}` в строке: скобка записывается как `}}`");
  EXPECT_EQ(
      syntaxError(R"(rule "a" when true then cancel because "{1 + 2")").Message,
      "подстановка не закрыта, ожидалась `}`");
  EXPECT_EQ(
      syntaxError(R"(rule "a" when true then cancel because "{}")").Message,
      "ожидалось выражение");
  EXPECT_EQ(
      syntaxError(R"(rule "a" when true then cancel because "{1 2}")").Message,
      "ожидалось `}`");
  EXPECT_EQ(
      syntaxError(R"(rule "a" when true then cancel because "{"x"}")").Message,
      "подстановка не закрыта, ожидалась `}`");
  EXPECT_EQ(
      syntaxError(R"(rule "{x}" when true then cancel because "y")").Message,
      "в названии правила подстановки не допускаются");
  EXPECT_EQ(
      syntaxError(
          R"(rule "a" when close[99999999999999999999] > 0 then cancel because "x")")
          .Message,
      "число вне диапазона");
}

TEST(Parser, InvalidUtf8) {
  const Diagnostic D = syntaxError(std::string("# ") + "\xC3\x28" + "\nrule");
  EXPECT_EQ(D.Message, "недопустимая последовательность байтов UTF-8");
  EXPECT_EQ(D.Range, (SourceRange{1, 3, 1, 4}));
}

TEST(Parser, StopsAtFirstError) {
  const ParseResult Result =
      parse(R"(rule "a" when ) then ? because "x" rule)");
  ASSERT_EQ(Result.Diagnostics.size(), 1U);
  EXPECT_EQ(Result.Diagnostics[0].Range.StartColumn, 15);
}

// ---- ограничения ----

TEST(Parser, LimitOnParamsAndRules) {
  std::string Params;
  for (int K = 0; K < 50; ++K)
    Params += "param p" + std::string(1, static_cast<char>('a' + K / 26)) +
              std::string(1, static_cast<char>('a' + K % 26)) + " = 1\n";
  const std::string Rule = "rule \"r\" when true then cancel because \"x\"\n";
  EXPECT_TRUE(parse(Params + Rule).Ast.has_value());
  const Diagnostic TooManyParams =
      syntaxError(Params + "param extra = 1\n" + Rule);
  EXPECT_EQ(TooManyParams.Message, "слишком много параметров: не более 50");
  EXPECT_EQ(TooManyParams.Range.StartLine, 51);

  std::string Rules;
  for (int K = 0; K < 100; ++K)
    Rules += Rule;
  EXPECT_TRUE(parse(Rules).Ast.has_value());
  const Diagnostic TooManyRules = syntaxError(Rules + Rule);
  EXPECT_EQ(TooManyRules.Message, "слишком много правил: не более 100");
  EXPECT_EQ(TooManyRules.Range.StartLine, 101);
}

TEST(Parser, LimitOnSourceSize) {
  const std::string Rule = "rule \"r\" when true then cancel because \"x\"\n";
  std::string Source = Rule;
  Source += "#" + std::string(64 * 1024 - Source.size() - 1, 'x');
  ASSERT_EQ(Source.size(), 64U * 1024U);
  EXPECT_TRUE(parse(Source).Ast.has_value());
  const Diagnostic TooLong = syntaxError(Source + "x");
  EXPECT_EQ(TooLong.Message, "текст длиннее 64 КиБ");
  EXPECT_EQ(TooLong.Range, (SourceRange{1, 1, 1, 2}));
}

TEST(Parser, DeepNestingIsRejectedNotOverflowed) {
  const std::string Prefix = "rule \"r\" when ";
  const std::string Suffix = " then cancel because \"x\"";
  EXPECT_TRUE(parse(Prefix + std::string(40, '(') + "true" +
                    std::string(40, ')') + Suffix)
                  .Ast.has_value());
  EXPECT_EQ(syntaxError(Prefix + std::string(20000, '(') + "true" +
                        std::string(20000, ')') + Suffix)
                .Message,
            "слишком глубокая вложенность выражения: не более 100 уровней");
  std::string Chain = "1";
  for (int K = 0; K < 20000; ++K)
    Chain += "+1";
  EXPECT_EQ(syntaxError(Prefix + Chain + Suffix).Message,
            "слишком глубокая вложенность выражения: не более 100 уровней");
}

TEST(Parser, TreeHeightIsBoundedThroughNestedParens) {
  // 99 уровней скобок, после каждой `)` цепочка `+1`: источник мал, а дерево
  // было бы в тысячи уровней.
  std::string Expr = "1";
  for (int Level = 1; Level <= 99; ++Level) {
    Expr = "(" + Expr + ")";
    for (int K = 0; K < 100 - Level; ++K)
      Expr += "+1";
  }
  const std::string Source =
      "rule \"r\" when " + Expr + " > 0 then cancel because \"x\"";
  ASSERT_LT(Source.size(), 64U * 1024U);
  EXPECT_EQ(syntaxError(Source).Message,
            "слишком глубокая вложенность выражения: не более 100 уровней");

  // Высота считается по узлам: цепочка из 50 слагаемых, обёрнутая в скобки,
  // проходит.
  std::string Chain = "a";
  for (int K = 0; K < 49; ++K)
    Chain += "+a";
  const ParseResult Ok =
      parse("rule \"r\" when ((" + Chain + ")) > 0 then cancel because \"x\"");
  ASSERT_TRUE(Ok.Ast.has_value());
  EXPECT_EQ(Ok.Ast->Rules[0].When->Height, 51);
}

TEST(Parser, HashInSubstitutionIsNotAComment) {
  // В тексте строки `#` обычный символ; в подстановке - недопустимый символ, а
  // не комментарий до конца строки.
  const ParseResult Text =
      parse(R"(rule "r" when true then cancel because "a # b {1}")");
  ASSERT_TRUE(Text.Ast.has_value());
  EXPECT_EQ(std::get<std::string>(Text.Ast->Rules[0].Because.Parts[0]),
            "a # b ");
  EXPECT_EQ(syntaxError("rule \"r\" when true then cancel because \"{1 # }\"")
                .Message,
            "недопустимый символ `#`");
}

TEST(Parser, CarriageReturnsAreStrippedFromStrings) {
  const ParseResult Result =
      parse("rule \"a\r\nb\" when true then cancel because \"x\r\n{1}\"\r\n");
  ASSERT_TRUE(Result.Ast.has_value());
  EXPECT_EQ(Result.Ast->Rules[0].Name, "a\nb");
  EXPECT_EQ(std::get<std::string>(Result.Ast->Rules[0].Because.Parts[0]),
            "x\n");
}

// ---- диапазоны ----

TEST(Parser, NodeRanges) {
  const ParseResult Result =
      parse("param a = 1\nrule \"r\" when (a + 2) * 3 > 4\n  then buy 5 lots "
            "limit close\n  because \"x\"");
  ASSERT_TRUE(Result.Ast.has_value());
  const Program &P = *Result.Ast;
  EXPECT_EQ(P.Params[0].Range, (SourceRange{1, 1, 1, 12}));
  const Rule &Rule = P.Rules[0];
  EXPECT_EQ(Rule.Range, (SourceRange{2, 1, 4, 14}));
  EXPECT_EQ(Rule.When->Range, (SourceRange{2, 15, 2, 30}));
  const auto &Cmp = std::get<Binary>(Rule.When->Node);
  const auto &Product = std::get<Binary>(Cmp.Left->Node);
  EXPECT_EQ(Cmp.Left->Range, (SourceRange{2, 15, 2, 26}));
  EXPECT_EQ(Product.Left->Range, (SourceRange{2, 15, 2, 22})); // со скобками
  EXPECT_EQ(std::get<Binary>(Product.Left->Node).Right->Range,
            (SourceRange{2, 20, 2, 21})); // литерал 2
  EXPECT_EQ(Rule.Then.Range, (SourceRange{3, 8, 3, 30}));
  EXPECT_EQ(Rule.Because.Range, (SourceRange{4, 11, 4, 14}));
}

} // namespace core::dsl
