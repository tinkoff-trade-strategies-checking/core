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
// Рекурсивные обходы дерева (проверка, интерпретатор, деструктор) не должны падать на тексте
// из 64 КиБ: вложенность скобок и вызовов при разборе и высота готового дерева (цепочки `1+1+…`) ограничены.
constexpr int kMaxDepth = 100;

struct Pos {
  int line;
  int column;
};

class Parser {
 public:
  explicit Parser(std::string_view source) : lexer_(source) {}

  Program parseProgram() {
    advance();
    Program program;
    while (isKeyword("param")) {
      if (program.params.size() == kMaxParams) fail(cur_, "слишком много параметров: не более 50");
      program.params.push_back(parseParam());
    }
    if (!isKeyword("rule")) fail(cur_, "ожидалось `param` или `rule`");
    while (isKeyword("rule")) {
      if (program.rules.size() == kMaxRules) fail(cur_, "слишком много правил: не более 100");
      program.rules.push_back(parseRule());
    }
    if (cur_.kind != TokenKind::End) fail(cur_, "ожидалось `rule` или конец текста");
    return program;
  }

 private:
  // ---- чтение лексем ----

  void advance() {
    prevEnd_ = {cur_.range.endLine, cur_.range.endColumn};
    if (substitution_ != nullptr) {
      cur_ = (*substitution_)[std::min(substitutionIndex_, substitution_->size() - 1)];
      ++substitutionIndex_;
    } else {
      cur_ = lexer_.next();
    }
  }

  bool isKeyword(std::string_view word) const { return cur_.kind == TokenKind::Keyword && cur_.text == word; }
  bool isPunct(std::string_view symbol) const { return cur_.kind == TokenKind::Punct && cur_.text == symbol; }

  [[noreturn]] static void failAt(const SourceRange& range, std::string message) {
    throw SyntaxError{Diagnostic{"E-SYN", Level::Error, range, std::move(message), {}}};
  }
  [[noreturn]] static void fail(const Token& at, std::string message) { failAt(at.range, std::move(message)); }

  void expectKeyword(std::string_view word) {
    if (!isKeyword(word)) fail(cur_, "ожидалось `" + std::string(word) + "`");
    advance();
  }
  void expectPunct(std::string_view symbol, const std::string& message) {
    if (!isPunct(symbol)) fail(cur_, message);
    advance();
  }

  static Pos startOf(const Token& token) { return {token.range.startLine, token.range.startColumn}; }
  SourceRange rangeFrom(Pos start) const { return {start.line, start.column, prevEnd_.line, prevEnd_.column}; }

  static constexpr const char* kTooDeep = "слишком глубокая вложенность выражения: не более 100 уровней";

  void checkDepth() const {
    if (depth_ > kMaxDepth) fail(cur_, kTooDeep);
  }

  // ---- числа ----

  double numberValue(const Token& token) const {
    double value = 0;
    const char* first = token.text.data();
    const char* last = first + token.text.size();
    const auto [end, error] = std::from_chars(first, last, value);
    if (error != std::errc() || end != last) fail(token, "число вне диапазона");
    return value;
  }

  std::int64_t integerValue(const Token& token) const {
    std::int64_t value = 0;
    const char* first = token.text.data();
    const char* last = first + token.text.size();
    const auto [end, error] = std::from_chars(first, last, value);
    if (error != std::errc() || end != last) fail(token, "число вне диапазона");
    return value;
  }

  // signed_num = [ "-" ] , number
  double parseSignedNumber() {
    bool negative = false;
    if (isPunct("-")) {
      negative = true;
      advance();
    }
    if (cur_.kind != TokenKind::Number) fail(cur_, "ожидалось число");
    const double value = numberValue(cur_);
    advance();
    return negative ? -value : value;
  }

  // ---- верхний уровень ----

  ParamDecl parseParam() {
    const Pos start = startOf(cur_);
    expectKeyword("param");
    if (cur_.kind != TokenKind::Ident) fail(cur_, "ожидалось имя параметра");
    ParamDecl param;
    param.name = cur_.text;
    advance();
    expectPunct("=", "ожидалось `=`");
    param.value = parseSignedNumber();
    if (isKeyword("range")) {
      advance();
      param.min = parseSignedNumber();
      expectPunct("..", "ожидалось `..`");
      param.max = parseSignedNumber();
      expectKeyword("step");
      if (cur_.kind != TokenKind::Number) fail(cur_, "ожидалось число");
      param.step = numberValue(cur_);
      advance();
    }
    param.range = rangeFrom(start);
    return param;
  }

  Rule parseRule() {
    const Pos start = startOf(cur_);
    expectKeyword("rule");
    if (cur_.kind != TokenKind::String) fail(cur_, "ожидалась строка с названием правила");
    Rule rule;
    for (const auto& part : cur_.parts) {
      if (part.isExpr) failAt(part.range, "в названии правила подстановки не допускаются");
      rule.name += part.text;
    }
    advance();
    expectKeyword("when");
    rule.when = parseExpr();
    expectKeyword("then");
    rule.action = parseAction();
    expectKeyword("because");
    rule.because = parseTemplate();
    rule.range = rangeFrom(start);
    return rule;
  }

  Action parseAction() {
    const Pos start = startOf(cur_);
    Action action;
    if (isKeyword("cancel")) {
      action.kind = Action::Kind::Cancel;
      advance();
      action.range = rangeFrom(start);
      return action;
    }
    if (isKeyword("buy")) {
      action.kind = Action::Kind::Buy;
    } else if (isKeyword("sell")) {
      action.kind = Action::Kind::Sell;
    } else {
      fail(cur_, "ожидалось `buy`, `sell` или `cancel`");
    }
    advance();

    // size = expr "lots" | expr "%" ( "equity" | "cash" | "position" ) | "all"
    if (isKeyword("all")) {
      action.sizeKind = SizeKind::All;
      advance();
    } else {
      action.size = parseExpr();
      if (isKeyword("lots")) {
        action.sizeKind = SizeKind::Lots;
        advance();
      } else if (isPunct("%")) {
        advance();
        if (isKeyword("equity")) {
          action.sizeKind = SizeKind::PercentEquity;
        } else if (isKeyword("cash")) {
          action.sizeKind = SizeKind::PercentCash;
        } else if (isKeyword("position")) {
          action.sizeKind = SizeKind::PercentPosition;
        } else {
          fail(cur_, "ожидалось `equity`, `cash` или `position`");
        }
        advance();
      } else {
        fail(cur_, "ожидалось `lots` или `%`");
      }
    }

    // order = "market" | "limit" expr | "stop" expr | "take_profit" expr
    if (isKeyword("market")) {
      advance();
    } else if (isKeyword("limit") || isKeyword("stop") || isKeyword("take_profit")) {
      action.orderKind = isKeyword("limit") ? OrderKind::Limit : isKeyword("stop") ? OrderKind::Stop : OrderKind::TakeProfit;
      advance();
      action.price = parseExpr();
    }
    // tif = "day" | "gtc"
    if (isKeyword("day") || isKeyword("gtc")) {
      action.day = isKeyword("day");
      advance();
    }
    action.range = rangeFrom(start);
    return action;
  }

  // Строка с подстановками. Выражения подстановок читаются из лексем, которые лексер уже собрал.
  StringTemplate parseTemplate() {
    if (cur_.kind != TokenKind::String) fail(cur_, "ожидалась строка с причиной");
    Token string = std::move(cur_);
    StringTemplate result;
    result.range = string.range;
    for (auto& part : string.parts) {
      if (!part.isExpr) {
        result.parts.emplace_back(std::move(part.text));
        continue;
      }
      substitution_ = &part.tokens;
      substitutionIndex_ = 0;
      advance();
      ExprPtr expr = parseExpr();
      if (cur_.kind != TokenKind::End) fail(cur_, "ожидалось `}`");
      substitution_ = nullptr;
      result.parts.emplace_back(std::move(expr));
    }
    cur_.range = string.range;  // prevEnd_ после advance() - конец строки
    advance();
    return result;
  }

  // ---- выражения ----

  static int heightOf(const ExprPtr& child) { return child ? child->height : 0; }

  // Высота узла - 1 + наибольшая высота потомков; больше kMaxDepth - ошибка. Только вложенность
  // скобок не ограничила бы высоту: `(…(1+1+1)+1+1…)` растёт цепочками внутри каждого уровня.
  ExprPtr make(auto&& node, const SourceRange& range) const {
    auto expr = std::make_unique<Expr>();
    expr->node = std::forward<decltype(node)>(node);
    expr->range = range;
    int children = 0;
    if (const auto* unary = std::get_if<Unary>(&expr->node)) {
      children = heightOf(unary->operand);
    } else if (const auto* binary = std::get_if<Binary>(&expr->node)) {
      children = std::max(heightOf(binary->left), heightOf(binary->right));
    } else if (const auto* call = std::get_if<Call>(&expr->node)) {
      for (const ExprPtr& arg : call->args) children = std::max(children, heightOf(arg));
    }
    expr->height = children + 1;
    if (expr->height > kMaxDepth) failAt(range, kTooDeep);
    return expr;
  }

  static SourceRange span(const Expr& first, const Expr& last) {
    return {first.range.startLine, first.range.startColumn, last.range.endLine, last.range.endColumn};
  }

  ExprPtr binary(std::string op, ExprPtr left, ExprPtr right) const {
    const SourceRange range = span(*left, *right);
    return make(Binary{std::move(op), std::move(left), std::move(right)}, range);
  }

  // expr = and_expr , { "or" , and_expr }
  ExprPtr parseExpr() {
    ++depth_;
    checkDepth();
    ExprPtr left = parseAnd();
    while (isKeyword("or")) {
      advance();
      left = binary("or", std::move(left), parseAnd());
    }
    --depth_;
    return left;
  }

  // and_expr = not_expr , { "and" , not_expr }
  ExprPtr parseAnd() {
    ExprPtr left = parseNot();
    while (isKeyword("and")) {
      advance();
      left = binary("and", std::move(left), parseNot());
    }
    return left;
  }

  // not_expr = [ "not" ] , cmp_expr
  ExprPtr parseNot() {
    if (!isKeyword("not")) return parseCmp();
    const Pos start = startOf(cur_);
    advance();
    ExprPtr operand = parseCmp();
    return make(Unary{'!', std::move(operand)}, rangeFrom(start));
  }

  // cmp_expr = add_expr , [ ( "<" | "<=" | ">" | ">=" | "==" | "!=" ) , add_expr ]
  ExprPtr parseCmp() {
    ExprPtr left = parseAdd();
    if (cur_.kind == TokenKind::Punct &&
        (cur_.text == "<" || cur_.text == "<=" || cur_.text == ">" || cur_.text == ">=" || cur_.text == "==" || cur_.text == "!=")) {
      std::string op = cur_.text;
      advance();
      return binary(std::move(op), std::move(left), parseAdd());
    }
    return left;
  }

  // add_expr = mul_expr , { ( "+" | "-" ) , mul_expr }
  ExprPtr parseAdd() {
    ExprPtr left = parseMul();
    while (isPunct("+") || isPunct("-")) {
      std::string op = cur_.text;
      advance();
      left = binary(std::move(op), std::move(left), parseMul());
    }
    return left;
  }

  // mul_expr = unary , { ( "*" | "/" ) , unary }
  ExprPtr parseMul() {
    ExprPtr left = parseUnary();
    while (isPunct("*") || isPunct("/")) {
      std::string op = cur_.text;
      advance();
      left = binary(std::move(op), std::move(left), parseUnary());
    }
    return left;
  }

  // unary = [ "-" ] , postfix
  ExprPtr parseUnary() {
    if (!isPunct("-")) return parsePostfix();
    const Pos start = startOf(cur_);
    advance();
    ExprPtr operand = parsePostfix();
    return make(Unary{'-', std::move(operand)}, rangeFrom(start));
  }

  // [ "[" , [ "-" ] , ( integer | ident ) , "]" ]
  std::optional<Offset> parseOffset() {
    if (!isPunct("[")) return std::nullopt;
    const Pos start = startOf(cur_);
    advance();
    Offset offset;
    if (isPunct("-")) {
      offset.negative = true;
      advance();
    }
    if (cur_.kind == TokenKind::Number) {
      if (!cur_.isInteger) fail(cur_, "ожидалось целое число или имя параметра");
      offset.value = integerValue(cur_);
    } else if (cur_.kind == TokenKind::Ident) {
      offset.value = cur_.text;
    } else {
      fail(cur_, "ожидалось целое число или имя параметра");
    }
    advance();
    expectPunct("]", "ожидалось `]`");
    offset.range = rangeFrom(start);
    return offset;
  }

  // postfix = ( ident | call ) , [ offset ] | number | "true" | "false" | "na" | "(" , expr , ")"
  ExprPtr parsePostfix() {
    const Pos start = startOf(cur_);
    // `position`, `cash`, `equity` - ключевые слова в действиях, но в выражениях это показатели портфеля.
    const bool portfolioName = isKeyword("position") || isKeyword("cash") || isKeyword("equity");
    if (cur_.kind == TokenKind::Number) {
      Number number{numberValue(cur_), cur_.text};
      const SourceRange range = cur_.range;
      advance();
      return make(std::move(number), range);
    }
    if (isKeyword("true") || isKeyword("false")) {
      const bool value = cur_.text == "true";
      const SourceRange range = cur_.range;
      advance();
      return make(Bool{value}, range);
    }
    if (isKeyword("na")) {
      const SourceRange range = cur_.range;
      advance();
      return make(Na{}, range);
    }
    if (isPunct("(")) {
      advance();
      ExprPtr inner = parseExpr();
      expectPunct(")", "ожидалось `)`");
      inner->range = rangeFrom(start);  // скобки входят в диапазон выражения
      return inner;
    }
    if (cur_.kind == TokenKind::Ident || portfolioName) {
      std::string name = cur_.text;
      advance();
      if (isPunct("(")) {
        advance();
        Call call;
        call.name = std::move(name);
        if (!isPunct(")")) {
          for (;;) {
            call.args.push_back(parseExpr());
            if (!isPunct(",")) break;
            advance();
          }
          expectPunct(")", "ожидалось `,` или `)`");
        } else {
          advance();
        }
        call.offset = parseOffset();
        return make(std::move(call), rangeFrom(start));
      }
      Ident ident;
      ident.name = std::move(name);
      ident.offset = parseOffset();
      return make(std::move(ident), rangeFrom(start));
    }
    fail(cur_, "ожидалось выражение");
  }

  Lexer lexer_;
  Token cur_;
  const std::vector<Token>* substitution_ = nullptr;  // не null - читаем лексемы подстановки, а не текст
  std::size_t substitutionIndex_ = 0;
  Pos prevEnd_{1, 1};
  int depth_ = 0;
};

}  // namespace

ParseResult parse(std::string_view source) {
  ParseResult result;
  try {
    if (source.size() > kMaxSourceBytes) {
      throw SyntaxError{Diagnostic{"E-SYN", Level::Error, {1, 1, 1, 2}, "текст длиннее 64 КиБ", {}}};
    }
    Parser parser(source);
    result.program = parser.parseProgram();
  } catch (SyntaxError& error) {
    result.program.reset();
    result.diagnostics.push_back(std::move(error.diagnostic));
  }
  return result;
}

}  // namespace core::dsl
