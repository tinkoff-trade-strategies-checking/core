#include "core/dsl/parser.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <system_error>
#include <utility>

#include "lexer.hpp"

namespace core::dsl {
namespace {

using detail::Lexer;
using detail::SyntaxError;
using detail::Token;
using detail::TokenKind;

constexpr std::size_t kMaxSourceBytes = 64 * 1024;
constexpr std::size_t kMaxRules = 100;
constexpr std::size_t kMaxParams = 50;
// Рекурсивные обходы дерева (проверка, интерпретатор, деструктор) не должны
// падать на тексте из 64 КиБ: вложенность скобок и вызовов при разборе и высота
// готового дерева (цепочки `1+1+…`) ограничены.
constexpr int kMaxDepth = 100;

struct Pos {
  int Line;
  int Column;
};

class Parser {
public:
  explicit Parser(std::string_view Source) : Lex(Source) {}

  Program parseProgram() {
    advance();
    Program Program;
    while (isKeyword("param")) {
      if (Program.Params.size() == kMaxParams)
        fail(Cur, "слишком много параметров: не более 50");
      Program.Params.push_back(parseParam());
    }
    if (!isKeyword("rule"))
      fail(Cur, "ожидалось `param` или `rule`");
    while (isKeyword("rule")) {
      if (Program.Rules.size() == kMaxRules)
        fail(Cur, "слишком много правил: не более 100");
      Program.Rules.push_back(parseRule());
    }
    if (Cur.Kind != TokenKind::End)
      fail(Cur, "ожидалось `rule` или конец текста");
    return Program;
  }

private:
  // ---- чтение лексем ----

  void advance() {
    PrevEnd = {Cur.Range.EndLine, Cur.Range.EndColumn};
    if (Substitution != nullptr) {
      Cur = (*Substitution)[std::min(SubstitutionIndex,
                                     Substitution->size() - 1)];
      ++SubstitutionIndex;
    } else {
      Cur = Lex.next();
    }
  }

  bool isKeyword(std::string_view Word) const {
    return Cur.Kind == TokenKind::Keyword && Cur.Text == Word;
  }
  bool isPunct(std::string_view Symbol) const {
    return Cur.Kind == TokenKind::Punct && Cur.Text == Symbol;
  }

  [[noreturn]] static void failAt(const SourceRange &Range,
                                  std::string Message) {
    throw SyntaxError{
        Diagnostic{"E-SYN", Level::Error, Range, std::move(Message), {}}};
  }
  [[noreturn]] static void fail(const Token &At, std::string Message) {
    failAt(At.Range, std::move(Message));
  }

  void expectKeyword(std::string_view Word) {
    if (!isKeyword(Word))
      fail(Cur, "ожидалось `" + std::string(Word) + "`");
    advance();
  }
  void expectPunct(std::string_view Symbol, const std::string &Message) {
    if (!isPunct(Symbol))
      fail(Cur, Message);
    advance();
  }

  static Pos startOf(const Token &Token) {
    return {Token.Range.StartLine, Token.Range.StartColumn};
  }
  SourceRange rangeFrom(Pos Start) const {
    return {Start.Line, Start.Column, PrevEnd.Line, PrevEnd.Column};
  }

  static constexpr const char *kTooDeep =
      "слишком глубокая вложенность выражения: не более 100 уровней";

  void checkDepth() const {
    if (Depth > kMaxDepth)
      fail(Cur, kTooDeep);
  }

  // ---- числа ----

  double numberValue(const Token &Token) const {
    double Value = 0;
    const char *First = Token.Text.data();
    const char *Last = First + Token.Text.size();
    const auto [end, error] = std::from_chars(First, Last, Value);
    if (error != std::errc() || end != Last)
      fail(Token, "число вне диапазона");
    return Value;
  }

  std::int64_t integerValue(const Token &Token) const {
    std::int64_t Value = 0;
    const char *First = Token.Text.data();
    const char *Last = First + Token.Text.size();
    const auto [end, error] = std::from_chars(First, Last, Value);
    if (error != std::errc() || end != Last)
      fail(Token, "число вне диапазона");
    return Value;
  }

  // signed_num = [ "-" ] , number
  double parseSignedNumber() {
    bool Negative = false;
    if (isPunct("-")) {
      Negative = true;
      advance();
    }
    if (Cur.Kind != TokenKind::Number)
      fail(Cur, "ожидалось число");
    const double Value = numberValue(Cur);
    advance();
    return Negative ? -Value : Value;
  }

  // ---- верхний уровень ----

  ParamDecl parseParam() {
    const Pos Start = startOf(Cur);
    expectKeyword("param");
    if (Cur.Kind != TokenKind::Ident)
      fail(Cur, "ожидалось имя параметра");
    ParamDecl Param;
    Param.Name = Cur.Text;
    advance();
    expectPunct("=", "ожидалось `=`");
    Param.Value = parseSignedNumber();
    if (isKeyword("range")) {
      advance();
      Param.Min = parseSignedNumber();
      expectPunct("..", "ожидалось `..`");
      Param.Max = parseSignedNumber();
      expectKeyword("step");
      if (Cur.Kind != TokenKind::Number)
        fail(Cur, "ожидалось число");
      Param.Step = numberValue(Cur);
      advance();
    }
    Param.Range = rangeFrom(Start);
    return Param;
  }

  Rule parseRule() {
    const Pos Start = startOf(Cur);
    expectKeyword("rule");
    if (Cur.Kind != TokenKind::String)
      fail(Cur, "ожидалась строка с названием правила");
    Rule Rule;
    for (const auto &Part : Cur.Parts) {
      if (Part.IsExpr)
        failAt(Part.Range, "в названии правила подстановки не допускаются");
      Rule.Name += Part.Text;
    }
    advance();
    expectKeyword("when");
    Rule.When = parseExpr();
    expectKeyword("then");
    Rule.Then = parseAction();
    expectKeyword("because");
    Rule.Because = parseTemplate();
    Rule.Range = rangeFrom(Start);
    return Rule;
  }

  Action parseAction() {
    const Pos Start = startOf(Cur);
    Action Action;
    if (isKeyword("cancel")) {
      Action.ActionKind = Action::Kind::Cancel;
      advance();
      Action.Range = rangeFrom(Start);
      return Action;
    }
    if (isKeyword("buy")) {
      Action.ActionKind = Action::Kind::Buy;
    } else if (isKeyword("sell")) {
      Action.ActionKind = Action::Kind::Sell;
    } else {
      fail(Cur, "ожидалось `buy`, `sell` или `cancel`");
    }
    advance();

    // size = expr "lots" | expr "%" ( "equity" | "cash" | "position" ) | "all"
    if (isKeyword("all")) {
      Action.SizeUnit = SizeKind::All;
      advance();
    } else {
      Action.Size = parseExpr();
      if (isKeyword("lots")) {
        Action.SizeUnit = SizeKind::Lots;
        advance();
      } else if (isPunct("%")) {
        advance();
        if (isKeyword("equity")) {
          Action.SizeUnit = SizeKind::PercentEquity;
        } else if (isKeyword("cash")) {
          Action.SizeUnit = SizeKind::PercentCash;
        } else if (isKeyword("position")) {
          Action.SizeUnit = SizeKind::PercentPosition;
        } else {
          fail(Cur, "ожидалось `equity`, `cash` или `position`");
        }
        advance();
      } else {
        fail(Cur, "ожидалось `lots` или `%`");
      }
    }

    // order = "market" | "limit" expr | "stop" expr | "take_profit" expr
    if (isKeyword("market")) {
      advance();
    } else if (isKeyword("limit") || isKeyword("stop") ||
               isKeyword("take_profit")) {
      Action.OrderMode = isKeyword("limit")  ? OrderKind::Limit
                         : isKeyword("stop") ? OrderKind::Stop
                                             : OrderKind::TakeProfit;
      advance();
      Action.Price = parseExpr();
    }
    // tif = "day" | "gtc"
    if (isKeyword("day") || isKeyword("gtc")) {
      Action.Day = isKeyword("day");
      advance();
    }
    Action.Range = rangeFrom(Start);
    return Action;
  }

  // Строка с подстановками. Выражения подстановок читаются из лексем, которые
  // лексер уже собрал.
  StringTemplate parseTemplate() {
    if (Cur.Kind != TokenKind::String)
      fail(Cur, "ожидалась строка с причиной");
    Token String = std::move(Cur);
    StringTemplate Result;
    Result.Range = String.Range;
    for (auto &Part : String.Parts) {
      if (!Part.IsExpr) {
        Result.Parts.emplace_back(std::move(Part.Text));
        continue;
      }
      Substitution = &Part.Tokens;
      SubstitutionIndex = 0;
      advance();
      ExprPtr Expr = parseExpr();
      if (Cur.Kind != TokenKind::End)
        fail(Cur, "ожидалось `}`");
      Substitution = nullptr;
      Result.Parts.emplace_back(std::move(Expr));
    }
    Cur.Range = String.Range; // prevEnd_ после advance() - конец строки
    advance();
    return Result;
  }

  // ---- выражения ----

  static int heightOf(const ExprPtr &Child) {
    return Child ? Child->Height : 0;
  }

  // Высота узла - 1 + наибольшая высота потомков; больше kMaxDepth - ошибка.
  // Только вложенность скобок не ограничила бы высоту: `(…(1+1+1)+1+1…)` растёт
  // цепочками внутри каждого уровня.
  ExprPtr make(auto &&Node, const SourceRange &Range) const {
    auto E = std::make_unique<Expr>();
    E->Node = std::forward<decltype(Node)>(Node);
    E->Range = Range;
    int Children = 0;
    if (const auto *UnaryNode = std::get_if<Unary>(&E->Node)) {
      Children = heightOf(UnaryNode->Operand);
    } else if (const auto *BinaryNode = std::get_if<Binary>(&E->Node)) {
      Children =
          std::max(heightOf(BinaryNode->Left), heightOf(BinaryNode->Right));
    } else if (const auto *CallNode = std::get_if<Call>(&E->Node)) {
      for (const ExprPtr &Arg : CallNode->Args)
        Children = std::max(Children, heightOf(Arg));
    }
    E->Height = Children + 1;
    if (E->Height > kMaxDepth)
      failAt(Range, kTooDeep);
    return E;
  }

  static SourceRange span(const Expr &First, const Expr &Last) {
    return {First.Range.StartLine, First.Range.StartColumn, Last.Range.EndLine,
            Last.Range.EndColumn};
  }

  ExprPtr binary(std::string Op, ExprPtr Left, ExprPtr Right) const {
    const SourceRange Range = span(*Left, *Right);
    return make(Binary{std::move(Op), std::move(Left), std::move(Right)},
                Range);
  }

  // expr = and_expr , { "or" , and_expr }
  ExprPtr parseExpr() {
    ++Depth;
    checkDepth();
    ExprPtr Left = parseAnd();
    while (isKeyword("or")) {
      advance();
      Left = binary("or", std::move(Left), parseAnd());
    }
    --Depth;
    return Left;
  }

  // and_expr = not_expr , { "and" , not_expr }
  ExprPtr parseAnd() {
    ExprPtr Left = parseNot();
    while (isKeyword("and")) {
      advance();
      Left = binary("and", std::move(Left), parseNot());
    }
    return Left;
  }

  // not_expr = [ "not" ] , cmp_expr
  ExprPtr parseNot() {
    if (!isKeyword("not"))
      return parseCmp();
    const Pos Start = startOf(Cur);
    advance();
    ExprPtr Operand = parseCmp();
    return make(Unary{'!', std::move(Operand)}, rangeFrom(Start));
  }

  // cmp_expr = add_expr , [ ( "<" | "<=" | ">" | ">=" | "==" | "!=" ) ,
  // add_expr ]
  ExprPtr parseCmp() {
    ExprPtr Left = parseAdd();
    if (Cur.Kind == TokenKind::Punct &&
        (Cur.Text == "<" || Cur.Text == "<=" || Cur.Text == ">" ||
         Cur.Text == ">=" || Cur.Text == "==" || Cur.Text == "!=")) {
      std::string Op = Cur.Text;
      advance();
      return binary(std::move(Op), std::move(Left), parseAdd());
    }
    return Left;
  }

  // add_expr = mul_expr , { ( "+" | "-" ) , mul_expr }
  ExprPtr parseAdd() {
    ExprPtr Left = parseMul();
    while (isPunct("+") || isPunct("-")) {
      std::string Op = Cur.Text;
      advance();
      Left = binary(std::move(Op), std::move(Left), parseMul());
    }
    return Left;
  }

  // mul_expr = unary , { ( "*" | "/" ) , unary }
  ExprPtr parseMul() {
    ExprPtr Left = parseUnary();
    while (isPunct("*") || isPunct("/")) {
      std::string Op = Cur.Text;
      advance();
      Left = binary(std::move(Op), std::move(Left), parseUnary());
    }
    return Left;
  }

  // unary = [ "-" ] , postfix
  ExprPtr parseUnary() {
    if (!isPunct("-"))
      return parsePostfix();
    const Pos Start = startOf(Cur);
    advance();
    ExprPtr Operand = parsePostfix();
    return make(Unary{'-', std::move(Operand)}, rangeFrom(Start));
  }

  // [ "[" , [ "-" ] , ( integer | ident ) , "]" ]
  std::optional<Offset> parseOffset() {
    if (!isPunct("["))
      return std::nullopt;
    const Pos Start = startOf(Cur);
    advance();
    Offset Offset;
    if (isPunct("-")) {
      Offset.Negative = true;
      advance();
    }
    if (Cur.Kind == TokenKind::Number) {
      if (!Cur.IsInteger)
        fail(Cur, "ожидалось целое число или имя параметра");
      Offset.Value = integerValue(Cur);
    } else if (Cur.Kind == TokenKind::Ident) {
      Offset.Value = Cur.Text;
    } else {
      fail(Cur, "ожидалось целое число или имя параметра");
    }
    advance();
    expectPunct("]", "ожидалось `]`");
    Offset.Range = rangeFrom(Start);
    return Offset;
  }

  // postfix = ( ident | call ) , [ offset ] | number | "true" | "false" | "na"
  // | "(" , expr , ")"
  ExprPtr parsePostfix() {
    const Pos Start = startOf(Cur);
    // `position`, `cash`, `equity` - ключевые слова в действиях, но в
    // выражениях это показатели портфеля.
    const bool PortfolioName =
        isKeyword("position") || isKeyword("cash") || isKeyword("equity");
    if (Cur.Kind == TokenKind::Number) {
      Number Number{numberValue(Cur), Cur.Text};
      const SourceRange Range = Cur.Range;
      advance();
      return make(std::move(Number), Range);
    }
    if (isKeyword("true") || isKeyword("false")) {
      const bool Value = Cur.Text == "true";
      const SourceRange Range = Cur.Range;
      advance();
      return make(Bool{Value}, Range);
    }
    if (isKeyword("na")) {
      const SourceRange Range = Cur.Range;
      advance();
      return make(Na{}, Range);
    }
    if (isPunct("(")) {
      advance();
      ExprPtr Inner = parseExpr();
      expectPunct(")", "ожидалось `)`");
      Inner->Range = rangeFrom(Start); // скобки входят в диапазон выражения
      return Inner;
    }
    if (Cur.Kind == TokenKind::Ident || PortfolioName) {
      std::string Name = Cur.Text;
      advance();
      if (isPunct("(")) {
        advance();
        Call Call;
        Call.Name = std::move(Name);
        if (!isPunct(")")) {
          for (;;) {
            Call.Args.push_back(parseExpr());
            if (!isPunct(","))
              break;
            advance();
          }
          expectPunct(")", "ожидалось `,` или `)`");
        } else {
          advance();
        }
        Call.BarOffset = parseOffset();
        return make(std::move(Call), rangeFrom(Start));
      }
      Ident Ident;
      Ident.Name = std::move(Name);
      Ident.BarOffset = parseOffset();
      return make(std::move(Ident), rangeFrom(Start));
    }
    fail(Cur, "ожидалось выражение");
  }

  Lexer Lex;
  Token Cur;
  const std::vector<Token> *Substitution =
      nullptr; // не null - читаем лексемы подстановки, а не текст
  std::size_t SubstitutionIndex = 0;
  Pos PrevEnd{1, 1};
  int Depth = 0;
};

} // namespace

ParseResult parse(std::string_view Source) {
  ParseResult Result;
  try {
    if (Source.size() > kMaxSourceBytes) {
      throw SyntaxError{Diagnostic{
          "E-SYN", Level::Error, {1, 1, 1, 2}, "текст длиннее 64 КиБ", {}}};
    }
    Parser Parser(Source);
    Result.Ast = Parser.parseProgram();
  } catch (SyntaxError &Error) {
    Result.Ast.reset();
    Result.Diagnostics.push_back(std::move(Error.Diag));
  }
  return Result;
}

} // namespace core::dsl
