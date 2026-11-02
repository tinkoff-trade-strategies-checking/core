#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/dsl/diagnostic.hpp"

// Дерево разбора DSL (dsl.md, «Грамматика»). Дерево только описывает текст: типы, имена и
// чтение будущего проверяются позже (checker), поэтому `close[-1]` - допустимое дерево.
// Диапазон каждого узла покрывает его текст целиком; у выражения в скобках - вместе со скобками.

namespace core::dsl {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

// Смещение `[k]`: отрицательное (`[-1]`) разбирается без ошибки, E-LA01 ставит проверка.
// range покрывает скобки `[` ... `]`.
struct Offset {
  bool negative = false;
  std::variant<std::int64_t, std::string> value;  // целое либо имя параметра
  SourceRange range;
};

struct Number {
  double value = 0;
  std::string text;  // как записано в тексте: линтеру нужен литерал
};
struct Bool {
  bool value = false;
};
struct Na {};
// Ряд, показатель портфеля или параметр: `close`, `position`, `fast`.
struct Ident {
  std::string name;
  std::optional<Offset> offset;
};
struct Call {
  std::string name;
  std::vector<ExprPtr> args;
  std::optional<Offset> offset;
};
struct Unary {
  char op = '-';  // '-' или '!' (not)
  ExprPtr operand;
};
// op: "or" "and" "<" "<=" ">" ">=" "==" "!=" "+" "-" "*" "/".
struct Binary {
  std::string op;
  ExprPtr left, right;
};

struct Expr {
  std::variant<Number, Bool, Na, Ident, Call, Unary, Binary> node;
  SourceRange range;
  int height = 1;  // высота поддерева (лист - 1); считает парсер, не больше 100
};

// Строка с подстановками: чередование текста (экранирование уже снято, `{{` -> `{`) и выражений.
// Соседние текстовые части склеены, пустых частей нет: `""` даёт пустой список parts.
struct StringTemplate {
  std::vector<std::variant<std::string, ExprPtr>> parts;
  SourceRange range;  // вместе с кавычками
};

enum class SizeKind { Lots, PercentEquity, PercentCash, PercentPosition, All };
enum class OrderKind { Market, Limit, Stop, TakeProfit };

struct Action {
  enum class Kind { Buy, Sell, Cancel } kind = Kind::Cancel;
  SizeKind sizeKind = SizeKind::Lots;  // у cancel не используется
  ExprPtr size;                        // null для `all` и cancel
  OrderKind orderKind = OrderKind::Market;
  ExprPtr price;               // null для market и cancel
  std::optional<bool> day;     // true - `day`, false - `gtc`, нет значения - срок не указан
  SourceRange range;
};

struct ParamDecl {
  std::string name;
  double value = 0;
  std::optional<double> min, max, step;  // заданы вместе: `range min..max step step`
  SourceRange range;
};

struct Rule {
  std::string name;
  ExprPtr when;
  Action action;
  StringTemplate because;
  SourceRange range;
};

struct Program {
  std::vector<ParamDecl> params;
  std::vector<Rule> rules;  // не меньше одного
};

}  // namespace core::dsl
