#include "core/dsl/checker.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace core::dsl {
namespace {

constexpr std::int64_t kMaxBars =
    5000; // смещение и период (dsl.md, «Где живёт»)

// Тип выражения. Any - `na` или выражение с уже найденной ошибкой: подходит
// куда угодно, чтобы одна ошибка не порождала лавину.
enum class Type { Num, Bool, Any };

enum class ArgKind { Value, Period };

struct Signature {
  std::string_view Name;
  std::vector<ArgKind> Args;
  Type Result;
  bool Indicator; // у вызова допустимо смещение
};

const std::vector<Signature> &signatures() {
  static const std::vector<Signature> kTable = {
      {"sma", {ArgKind::Value, ArgKind::Period}, Type::Num, true},
      {"ema", {ArgKind::Value, ArgKind::Period}, Type::Num, true},
      {"rsi", {ArgKind::Value, ArgKind::Period}, Type::Num, true},
      {"stdev", {ArgKind::Value, ArgKind::Period}, Type::Num, true},
      {"highest", {ArgKind::Value, ArgKind::Period}, Type::Num, true},
      {"lowest", {ArgKind::Value, ArgKind::Period}, Type::Num, true},
      {"change", {ArgKind::Value, ArgKind::Period}, Type::Num, true},
      {"atr", {ArgKind::Period}, Type::Num, true},
      {"crossover", {ArgKind::Value, ArgKind::Value}, Type::Bool, true},
      {"crossunder", {ArgKind::Value, ArgKind::Value}, Type::Bool, true},
      {"abs", {ArgKind::Value}, Type::Num, false},
      {"sqrt", {ArgKind::Value}, Type::Num, false},
      {"min", {ArgKind::Value, ArgKind::Value}, Type::Num, false},
      {"max", {ArgKind::Value, ArgKind::Value}, Type::Num, false},
  };
  return kTable;
}

const Signature *findSignature(std::string_view Name) {
  for (const Signature &Signature : signatures()) {
    if (Signature.Name == Name)
      return &Signature;
  }
  return nullptr;
}

bool isPriceSeries(std::string_view Name) {
  return Name == "open" || Name == "high" || Name == "low" || Name == "close";
}
bool isSeries(std::string_view Name) {
  return isPriceSeries(Name) || Name == "volume";
}
bool isPortfolioName(std::string_view Name) {
  return Name == "position" || Name == "entry_price" || Name == "cash" ||
         Name == "equity" || Name == "bars_in_position";
}
bool isBuiltinName(std::string_view Name) {
  return isSeries(Name) || isPortfolioName(Name) ||
         findSignature(Name) != nullptr;
}

bool isWhole(double X) { return std::floor(X) == X; }

// Число в тексте сообщений: целые без дробной части.
std::string show(double X) {
  if (isWhole(X) && std::fabs(X) < 1e15)
    return std::to_string(static_cast<std::int64_t>(X));
  std::string Text = std::to_string(X);
  while (!Text.empty() && Text.back() == '0')
    Text.pop_back();
  if (!Text.empty() && Text.back() == '.')
    Text.pop_back();
  return Text;
}

struct ParamInfo {
  const ParamDecl *Decl;
  std::size_t Index;
};

class Checker {
public:
  explicit Checker(const Program &Program) : Prog(Program) {}

  CheckResult run() {
    collectParams();
    for (const Rule &Rule : Prog.Rules)
      checkRule(Rule);

    CheckResult Result;
    for (std::size_t I = 0; I < Resolved.size(); ++I)
      Resolved[I].Integer = IntegerUse[I];
    Result.Params = std::move(Resolved);
    Result.Diagnostics = std::move(Diagnostics);
    Result.Severity = overallLevel(Result.Diagnostics);
    return Result;
  }

private:
  // ---- диагностики ----

  void report(std::string Code, Level Level, const SourceRange &Range,
              std::string Message) {
    Diagnostics.push_back(
        Diagnostic{std::move(Code), Level, Range, std::move(Message), {}});
  }
  void typeError(const SourceRange &Range, std::string Message) {
    report("E-TYPE", Level::Error, Range, std::move(Message));
  }

  // ---- параметры ----

  void collectParams() {
    for (const ParamDecl &Decl : Prog.Params) {
      if (isBuiltinName(Decl.Name)) {
        typeError(Decl.Range,
                  "имя параметра `" + Decl.Name + "` занято встроенным именем");
        continue;
      }
      if (Params.count(Decl.Name) != 0) {
        typeError(Decl.Range, "параметр `" + Decl.Name + "` уже объявлен");
        continue;
      }
      checkParamRange(Decl);
      Params[Decl.Name] = ParamInfo{&Decl, Resolved.size()};
      Resolved.push_back(ResolvedParam{Decl.Name, Decl.Value, Decl.Min,
                                       Decl.Max, Decl.Step, false});
      IntegerUse.push_back(false);
    }
  }

  void checkParamRange(const ParamDecl &Decl) {
    if (!Decl.Min || !Decl.Max || !Decl.Step)
      return;
    const std::string Name = "`" + Decl.Name + "`";
    if (*Decl.Min > *Decl.Max) {
      typeError(Decl.Range, "параметр " + Name +
                                ": нижняя граница диапазона больше верхней");
    } else if (Decl.Value < *Decl.Min || Decl.Value > *Decl.Max) {
      typeError(Decl.Range, "параметр " + Name +
                                ": значение по умолчанию вне диапазона " +
                                show(*Decl.Min) + ".." + show(*Decl.Max));
    }
    if (!(*Decl.Step > 0))
      typeError(Decl.Range, "параметр " + Name + ": шаг должен быть больше 0");
  }

  // Параметр в целочисленной позиции. lowest - наименьшее допустимое значение
  // позиции: 1 для периода, 0 для смещения. Ошибки позиции `offset` - E-LA02
  // (отрицательные значения), прочие - E-TYPE.
  void checkIntegerParam(const ParamInfo &Info, const SourceRange &Range,
                         bool IsOffset) {
    const ParamDecl &Decl = *Info.Decl;
    IntegerUse[Info.Index] = true;
    const std::string Name = "`" + Decl.Name + "`";
    const std::string Role = IsOffset ? "смещении" : "периоде";

    if (!isWhole(Decl.Value) || (Decl.Min && !isWhole(*Decl.Min)) ||
        (Decl.Max && !isWhole(*Decl.Max)) ||
        (Decl.Step && !isWhole(*Decl.Step))) {
      typeError(Range, "параметр " + Name + " в " + Role +
                           " должен быть целым вместе с диапазоном и шагом");
      return;
    }
    const double Low = std::min(Decl.Value, Decl.Min.value_or(Decl.Value));
    const double High = std::max(Decl.Value, Decl.Max.value_or(Decl.Value));
    if (IsOffset) {
      if (Low < 0) {
        report("E-LA02", Level::Error, Range,
               "чтение будущего: диапазон параметра " + Name +
                   " допускает отрицательные значения (" + show(Low) +
                   "), смещение не может быть отрицательным");
        return;
      }
    } else if (Low < 1) {
      typeError(Range,
                "параметр " + Name + " в периоде допускает значения меньше 1");
      return;
    }
    if (High > static_cast<double>(kMaxBars)) {
      typeError(Range, "параметр " + Name + " в " + Role +
                           " допускает значения больше " +
                           std::to_string(kMaxBars) + " баров");
    }
  }

  const ParamInfo *findParam(const std::string &Name) const {
    const auto Found = Params.find(Name);
    return Found == Params.end() ? nullptr : &Found->second;
  }

  // ---- правила ----

  void checkRule(const Rule &Rule) {
    const Type When = checkExpr(*Rule.When);
    if (When == Type::Num)
      typeError(Rule.When->Range,
                "условие `when` должно быть логическим, а не числом");
    checkAction(Rule.Then);
    checkBecause(Rule.Because);
  }

  void checkAction(const Action &Action) {
    if (Action.ActionKind == Action::Kind::Cancel)
      return;
    if (Action.ActionKind == Action::Kind::Buy &&
        Action.SizeUnit == SizeKind::All) {
      typeError(Action.Range,
                "`buy all` недопустимо: `all` есть только у `sell`");
    }
    if (Action.Size)
      expectNumber(*Action.Size, "количество");
    if (Action.Price)
      expectNumber(*Action.Price, "цена");
  }

  void expectNumber(const Expr &E, const char *What) {
    if (checkExpr(E) == Type::Bool) {
      typeError(E.Range, std::string(What) +
                             " должна быть числом, а не логическим значением");
    }
  }

  void checkBecause(const StringTemplate &Because) {
    bool Blank = true;
    for (const auto &Part : Because.Parts) {
      if (const auto *Text = std::get_if<std::string>(&Part)) {
        if (Text->find_first_not_of(" \t\r\n") != std::string::npos)
          Blank = false;
      } else {
        Blank = false;
        checkExpr(*std::get<ExprPtr>(Part)); // любое значение подставимо
      }
    }
    if (Blank)
      report("E-BEC", Level::Error, Because.Range,
             "у правила нет причины: `because` не может быть пустым");
  }

  // ---- выражения ----

  Type checkExpr(const Expr &E) {
    struct Visitor {
      Checker &Self;
      const Expr &E;
      Type operator()(const Number &) const { return Type::Num; }
      Type operator()(const Bool &) const { return Type::Bool; }
      Type operator()(const Na &) const { return Type::Any; }
      Type operator()(const Ident &Ident) const {
        return Self.checkIdent(Ident, E);
      }
      Type operator()(const Call &Call) const {
        return Self.checkCall(Call, E);
      }
      Type operator()(const Unary &Unary) const {
        return Self.checkUnary(Unary, E);
      }
      Type operator()(const Binary &Binary) const {
        return Self.checkBinary(Binary, E);
      }
    };
    return std::visit(Visitor{*this, E}, E.Node);
  }

  Type checkIdent(const Ident &Ident, const Expr &E) {
    const bool Series = isSeries(Ident.Name);
    if (!Series && !isPortfolioName(Ident.Name) &&
        findParam(Ident.Name) == nullptr) {
      typeError(E.Range, "неизвестное имя `" + Ident.Name + "`");
      return Type::Any;
    }
    if (Ident.BarOffset)
      checkOffset(*Ident.BarOffset, Series);
    return Type::Num;
  }

  void checkOffset(const Offset &Offset, bool Allowed) {
    if (!Allowed) {
      typeError(Offset.Range, "смещение допускается только у рядов (open, "
                              "high, low, close, volume) и индикаторов");
      return;
    }
    if (const auto *Number = std::get_if<std::int64_t>(&Offset.Value)) {
      if (Offset.Negative) {
        if (*Number != 0) {
          report("E-LA01", Level::Error, Offset.Range,
                 "чтение будущего: отрицательное смещение -" +
                     std::to_string(*Number) + " берёт данные следующих баров");
        }
        return;
      }
      if (*Number > kMaxBars) {
        typeError(Offset.Range, "смещение " + std::to_string(*Number) +
                                    " больше " + std::to_string(kMaxBars) +
                                    " баров");
      }
      return;
    }
    const std::string &Name = std::get<std::string>(Offset.Value);
    const ParamInfo *Info = findParam(Name);
    if (Info == nullptr) {
      if (isBuiltinName(Name)) {
        typeError(Offset.Range,
                  "смещение может быть целым числом или параметром, а не `" +
                      Name + "`");
      } else {
        typeError(Offset.Range, "неизвестное имя `" + Name + "`");
      }
      return;
    }
    if (Offset.Negative) {
      report("E-LA01", Level::Error, Offset.Range,
             "чтение будущего: отрицательное смещение -" + Name +
                 " берёт данные следующих баров");
      return;
    }
    checkIntegerParam(*Info, Offset.Range, true);
  }

  Type checkCall(const Call &Call, const Expr &E) {
    const Signature *Signature = findSignature(Call.Name);
    if (Signature == nullptr) {
      typeError(E.Range, "неизвестная функция `" + Call.Name + "`");
      for (const ExprPtr &Arg : Call.Args)
        checkExpr(*Arg);
      return Type::Any;
    }
    if (Call.Args.size() != Signature->Args.size()) {
      typeError(E.Range, "функция `" + Call.Name + "` принимает аргументов: " +
                             std::to_string(Signature->Args.size()) +
                             ", передано " + std::to_string(Call.Args.size()));
      for (const ExprPtr &Arg : Call.Args)
        checkExpr(*Arg);
    } else {
      for (std::size_t I = 0; I < Call.Args.size(); ++I) {
        const Expr &Arg = *Call.Args[I];
        if (Signature->Args[I] == ArgKind::Period) {
          checkPeriod(Arg);
        } else if (checkExpr(Arg) == Type::Bool) {
          typeError(Arg.Range,
                    "аргумент функции `" + Call.Name +
                        "` должен быть числом, а не логическим значением");
        }
      }
    }
    if (Call.BarOffset)
      checkOffset(*Call.BarOffset, Signature->Indicator);
    return Signature->Result;
  }

  // Период: целый литерал 1..5000 либо целочисленный параметр (dsl.md,
  // «Функции»).
  void checkPeriod(const Expr &Arg) {
    if (const auto *NumberNode = std::get_if<Number>(&Arg.Node)) {
      if (!isWhole(NumberNode->Value)) {
        typeError(Arg.Range,
                  "период должен быть целым числом, а не " + NumberNode->Text);
      } else if (NumberNode->Value < 1 ||
                 NumberNode->Value > static_cast<double>(kMaxBars)) {
        typeError(Arg.Range, "период " + NumberNode->Text +
                                 " вне пределов 1.." +
                                 std::to_string(kMaxBars));
      }
      return;
    }
    if (const auto *IdentNode = std::get_if<Ident>(&Arg.Node);
        IdentNode != nullptr && !IdentNode->BarOffset) {
      if (const ParamInfo *Info = findParam(IdentNode->Name)) {
        checkIntegerParam(*Info, Arg.Range, false);
        return;
      }
      if (!isBuiltinName(IdentNode->Name)) {
        typeError(Arg.Range, "неизвестное имя `" + IdentNode->Name + "`");
        return;
      }
    }
    typeError(Arg.Range,
              "период должен быть целым числом или целочисленным параметром");
    checkExpr(Arg);
  }

  Type checkUnary(const Unary &Unary, const Expr &E) {
    const Type Operand = checkExpr(*Unary.Operand);
    if (Unary.Op == '!') {
      if (Operand == Type::Num)
        typeError(E.Range,
                  "`not` применяется к логическому значению, а не к числу");
      return Type::Bool;
    }
    if (Operand == Type::Bool)
      typeError(
          E.Range,
          "унарный минус применяется к числу, а не к логическому значению");
    return Type::Num;
  }

  Type checkBinary(const Binary &Binary, const Expr &E) {
    const Type Left = checkExpr(*Binary.Left);
    const Type Right = checkExpr(*Binary.Right);
    const std::string &Op = Binary.Op;

    if (Op == "and" || Op == "or") {
      if (Left == Type::Num || Right == Type::Num) {
        typeError(E.Range,
                  "`" + Op + "` соединяет логические значения, а не числа");
      }
      return Type::Bool;
    }
    if (Op == "==" || Op == "!=") {
      if ((Left == Type::Num && Right == Type::Bool) ||
          (Left == Type::Bool && Right == Type::Num)) {
        typeError(E.Range, "нельзя сравнивать число с логическим значением");
      } else {
        warnAbsoluteLevel(Binary, E);
      }
      return Type::Bool;
    }
    if (Op == "<" || Op == "<=" || Op == ">" || Op == ">=") {
      if (Left == Type::Bool || Right == Type::Bool) {
        typeError(E.Range,
                  "`" + Op + "` сравнивает числа, а не логические значения");
      } else {
        warnAbsoluteLevel(Binary, E);
      }
      return Type::Bool;
    }
    // + - * /
    if (Left == Type::Bool || Right == Type::Bool) {
      typeError(E.Range,
                "арифметика `" + Op +
                    "` применяется к числам, а не к логическим значениям");
    }
    return Type::Num;
  }

  // ---- W-LA01 ----

  // `open`/`high`/`low`/`close` со смещением или без.
  static bool isPriceSeriesExpr(const Expr &E) {
    const auto *IdentNode = std::get_if<Ident>(&E.Node);
    return IdentNode != nullptr && isPriceSeries(IdentNode->Name);
  }

  // Цена либо `sma`/`ema`/`highest`/`lowest` от цены.
  static bool isPriceLike(const Expr &E) {
    if (isPriceSeriesExpr(E))
      return true;
    const auto *CallNode = std::get_if<Call>(&E.Node);
    if (CallNode == nullptr || CallNode->Args.empty())
      return false;
    const std::string &Name = CallNode->Name;
    return (Name == "sma" || Name == "ema" || Name == "highest" ||
            Name == "lowest") &&
           isPriceSeriesExpr(*CallNode->Args[0]);
  }

  // Выражение только из чисел и параметров.
  bool isConstant(const Expr &E) const {
    if (std::holds_alternative<Number>(E.Node))
      return true;
    if (const auto *IdentNode = std::get_if<Ident>(&E.Node))
      return !IdentNode->BarOffset && findParam(IdentNode->Name) != nullptr;
    if (const auto *UnaryNode = std::get_if<Unary>(&E.Node))
      return UnaryNode->Op == '-' && isConstant(*UnaryNode->Operand);
    if (const auto *BinaryNode = std::get_if<Binary>(&E.Node)) {
      const bool Arithmetic = BinaryNode->Op == "+" || BinaryNode->Op == "-" ||
                              BinaryNode->Op == "*" || BinaryNode->Op == "/";
      return Arithmetic && isConstant(*BinaryNode->Left) &&
             isConstant(*BinaryNode->Right);
    }
    return false;
  }

  void warnAbsoluteLevel(const Binary &Binary, const Expr &E) {
    const bool Absolute =
        (isPriceLike(*Binary.Left) && isConstant(*Binary.Right)) ||
        (isConstant(*Binary.Left) && isPriceLike(*Binary.Right));
    if (Absolute) {
      report("W-LA01", Level::Warning, E.Range,
             "цена сравнивается с абсолютным уровнем: такой уровень обычно "
             "подобран по известной истории");
    }
  }

  const Program &Prog;
  std::map<std::string, ParamInfo>
      Params; // упорядоченный контейнер: обход не зависит от адресов (INV-4)
  std::vector<ResolvedParam> Resolved;
  std::vector<bool> IntegerUse;
  std::vector<Diagnostic> Diagnostics;
};

} // namespace

CheckResult check(const Program &Program) { return Checker(Program).run(); }

Level overallLevel(const std::vector<Diagnostic> &Diagnostics) {
  Level Worst = Level::Ok;
  for (const Diagnostic &Diagnostic : Diagnostics)
    Worst = std::max(Worst, Diagnostic.Severity);
  return Worst;
}

} // namespace core::dsl
