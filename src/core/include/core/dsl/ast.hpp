#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/dsl/diagnostic.hpp"

// Дерево разбора DSL (dsl.md, «Грамматика»). Дерево только описывает текст:
// типы, имена и чтение будущего проверяются позже (checker), поэтому
// `close[-1]` - допустимое дерево. Диапазон каждого узла покрывает его текст
// целиком; у выражения в скобках - вместе со скобками.

namespace core::dsl {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

// Смещение `[k]`: отрицательное (`[-1]`) разбирается без ошибки, E-LA01 ставит
// проверка. range покрывает скобки `[` ... `]`.
struct Offset {
  bool Negative = false;
  std::variant<std::int64_t, std::string> Value; // целое либо имя параметра
  SourceRange Range;
};

struct Number {
  double Value = 0;
  std::string Text; // как записано в тексте: линтеру нужен литерал
};
struct Bool {
  bool Value = false;
};
struct Na {};
// Ряд, показатель портфеля или параметр: `close`, `position`, `fast`.
struct Ident {
  std::string Name;
  std::optional<Offset> BarOffset;
};
struct Call {
  std::string Name;
  std::vector<ExprPtr> Args;
  std::optional<Offset> BarOffset;
};
struct Unary {
  char Op = '-'; // '-' или '!' (not)
  ExprPtr Operand;
};
// op: "or" "and" "<" "<=" ">" ">=" "==" "!=" "+" "-" "*" "/".
struct Binary {
  std::string Op;
  ExprPtr Left, Right;
};

struct Expr {
  std::variant<Number, Bool, Na, Ident, Call, Unary, Binary> Node;
  SourceRange Range;
  int Height = 1; // высота поддерева (лист - 1); считает парсер, не больше 100
};

// Строка с подстановками: чередование текста (экранирование уже снято, `{{` ->
// `{`) и выражений. Соседние текстовые части склеены, пустых частей нет: `""`
// даёт пустой список parts.
struct StringTemplate {
  std::vector<std::variant<std::string, ExprPtr>> Parts;
  SourceRange Range; // вместе с кавычками
};

enum class SizeKind { Lots, PercentEquity, PercentCash, PercentPosition, All };
enum class OrderKind { Market, Limit, Stop, TakeProfit };

struct Action {
  enum class Kind { Buy, Sell, Cancel } ActionKind = Kind::Cancel;
  SizeKind SizeUnit = SizeKind::Lots; // у cancel не используется
  ExprPtr Size;                       // null для `all` и cancel
  OrderKind OrderMode = OrderKind::Market;
  ExprPtr Price; // null для market и cancel
  std::optional<bool>
      Day; // true - `day`, false - `gtc`, нет значения - срок не указан
  SourceRange Range;
};

struct ParamDecl {
  std::string Name;
  double Value = 0;
  std::optional<double> Min, Max,
      Step; // заданы вместе: `range min..max step step`
  SourceRange Range;
};

struct Rule {
  std::string Name;
  ExprPtr When;
  Action Then;
  StringTemplate Because;
  SourceRange Range;
};

struct Program {
  std::vector<ParamDecl> Params;
  std::vector<Rule> Rules; // не меньше одного
};

} // namespace core::dsl
