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

constexpr std::int64_t kMaxBars = 5000;  // смещение и период (dsl.md, «Где живёт»)

// Тип выражения. Any - `na` или выражение с уже найденной ошибкой: подходит куда угодно,
// чтобы одна ошибка не порождала лавину.
enum class Type { Num, Bool, Any };

enum class ArgKind { Value, Period };

struct Signature {
  std::string_view name;
  std::vector<ArgKind> args;
  Type result;
  bool indicator;  // у вызова допустимо смещение
};

const std::vector<Signature>& signatures() {
  static const std::vector<Signature> table = {
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
  return table;
}

const Signature* findSignature(std::string_view name) {
  for (const Signature& signature : signatures()) {
    if (signature.name == name) return &signature;
  }
  return nullptr;
}

bool isPriceSeries(std::string_view name) {
  return name == "open" || name == "high" || name == "low" || name == "close";
}
bool isSeries(std::string_view name) { return isPriceSeries(name) || name == "volume"; }
bool isPortfolioName(std::string_view name) {
  return name == "position" || name == "entry_price" || name == "cash" || name == "equity" ||
         name == "bars_in_position";
}
bool isBuiltinName(std::string_view name) {
  return isSeries(name) || isPortfolioName(name) || findSignature(name) != nullptr;
}

bool isWhole(double x) { return std::floor(x) == x; }

// Число в тексте сообщений: целые без дробной части.
std::string show(double x) {
  if (isWhole(x) && std::fabs(x) < 1e15) return std::to_string(static_cast<std::int64_t>(x));
  std::string text = std::to_string(x);
  while (!text.empty() && text.back() == '0') text.pop_back();
  if (!text.empty() && text.back() == '.') text.pop_back();
  return text;
}

struct ParamInfo {
  const ParamDecl* decl;
  std::size_t index;
};

class Checker {
 public:
  explicit Checker(const Program& program) : program_(program) {}

  CheckResult run() {
    collectParams();
    for (const Rule& rule : program_.rules) checkRule(rule);

    CheckResult result;
    for (std::size_t i = 0; i < resolved_.size(); ++i) resolved_[i].integer = integerUse_[i];
    result.params = std::move(resolved_);
    result.diagnostics = std::move(diagnostics_);
    result.level = overallLevel(result.diagnostics);
    return result;
  }

 private:
  // ---- диагностики ----

  void report(std::string code, Level level, const SourceRange& range, std::string message) {
    diagnostics_.push_back(Diagnostic{std::move(code), level, range, std::move(message), {}});
  }
  void typeError(const SourceRange& range, std::string message) {
    report("E-TYPE", Level::Error, range, std::move(message));
  }

  // ---- параметры ----

  void collectParams() {
    for (const ParamDecl& decl : program_.params) {
      if (isBuiltinName(decl.name)) {
        typeError(decl.range, "имя параметра `" + decl.name + "` занято встроенным именем");
        continue;
      }
      if (params_.count(decl.name) != 0) {
        typeError(decl.range, "параметр `" + decl.name + "` уже объявлен");
        continue;
      }
      checkParamRange(decl);
      params_[decl.name] = ParamInfo{&decl, resolved_.size()};
      resolved_.push_back(ResolvedParam{decl.name, decl.value, decl.min, decl.max, decl.step, false});
      integerUse_.push_back(false);
    }
  }

  void checkParamRange(const ParamDecl& decl) {
    if (!decl.min || !decl.max || !decl.step) return;
    const std::string name = "`" + decl.name + "`";
    if (*decl.min > *decl.max) {
      typeError(decl.range, "параметр " + name + ": нижняя граница диапазона больше верхней");
    } else if (decl.value < *decl.min || decl.value > *decl.max) {
      typeError(decl.range, "параметр " + name + ": значение по умолчанию вне диапазона " +
                                show(*decl.min) + ".." + show(*decl.max));
    }
    if (!(*decl.step > 0)) typeError(decl.range, "параметр " + name + ": шаг должен быть больше 0");
  }

  // Параметр в целочисленной позиции. lowest - наименьшее допустимое значение позиции:
  // 1 для периода, 0 для смещения. Ошибки позиции `offset` - E-LA02 (отрицательные значения), прочие - E-TYPE.
  void checkIntegerParam(const ParamInfo& info, const SourceRange& range, bool isOffset) {
    const ParamDecl& decl = *info.decl;
    integerUse_[info.index] = true;
    const std::string name = "`" + decl.name + "`";
    const std::string role = isOffset ? "смещении" : "периоде";

    if (!isWhole(decl.value) || (decl.min && !isWhole(*decl.min)) || (decl.max && !isWhole(*decl.max)) ||
        (decl.step && !isWhole(*decl.step))) {
      typeError(range, "параметр " + name + " в " + role +
                           " должен быть целым вместе с диапазоном и шагом");
      return;
    }
    const double low = std::min(decl.value, decl.min.value_or(decl.value));
    const double high = std::max(decl.value, decl.max.value_or(decl.value));
    if (isOffset) {
      if (low < 0) {
        report("E-LA02", Level::Error, range,
               "чтение будущего: диапазон параметра " + name + " допускает отрицательные значения (" +
                   show(low) + "), смещение не может быть отрицательным");
        return;
      }
    } else if (low < 1) {
      typeError(range, "параметр " + name + " в периоде допускает значения меньше 1");
      return;
    }
    if (high > static_cast<double>(kMaxBars)) {
      typeError(range, "параметр " + name + " в " + role + " допускает значения больше " +
                           std::to_string(kMaxBars) + " баров");
    }
  }

  const ParamInfo* findParam(const std::string& name) const {
    const auto found = params_.find(name);
    return found == params_.end() ? nullptr : &found->second;
  }

  // ---- правила ----

  void checkRule(const Rule& rule) {
    const Type when = checkExpr(*rule.when);
    if (when == Type::Num) typeError(rule.when->range, "условие `when` должно быть логическим, а не числом");
    checkAction(rule.action);
    checkBecause(rule.because);
  }

  void checkAction(const Action& action) {
    if (action.kind == Action::Kind::Cancel) return;
    if (action.kind == Action::Kind::Buy && action.sizeKind == SizeKind::All) {
      typeError(action.range, "`buy all` недопустимо: `all` есть только у `sell`");
    }
    if (action.size) expectNumber(*action.size, "количество");
    if (action.price) expectNumber(*action.price, "цена");
  }

  void expectNumber(const Expr& expr, const char* what) {
    if (checkExpr(expr) == Type::Bool) {
      typeError(expr.range, std::string(what) + " должна быть числом, а не логическим значением");
    }
  }

  void checkBecause(const StringTemplate& because) {
    bool blank = true;
    for (const auto& part : because.parts) {
      if (const auto* text = std::get_if<std::string>(&part)) {
        if (text->find_first_not_of(" \t\r\n") != std::string::npos) blank = false;
      } else {
        blank = false;
        checkExpr(*std::get<ExprPtr>(part));  // любое значение подставимо
      }
    }
    if (blank) report("E-BEC", Level::Error, because.range, "у правила нет причины: `because` не может быть пустым");
  }

  // ---- выражения ----

  Type checkExpr(const Expr& expr) {
    struct Visitor {
      Checker& self;
      const Expr& expr;
      Type operator()(const Number&) const { return Type::Num; }
      Type operator()(const Bool&) const { return Type::Bool; }
      Type operator()(const Na&) const { return Type::Any; }
      Type operator()(const Ident& ident) const { return self.checkIdent(ident, expr); }
      Type operator()(const Call& call) const { return self.checkCall(call, expr); }
      Type operator()(const Unary& unary) const { return self.checkUnary(unary, expr); }
      Type operator()(const Binary& binary) const { return self.checkBinary(binary, expr); }
    };
    return std::visit(Visitor{*this, expr}, expr.node);
  }

  Type checkIdent(const Ident& ident, const Expr& expr) {
    const bool series = isSeries(ident.name);
    if (!series && !isPortfolioName(ident.name) && findParam(ident.name) == nullptr) {
      typeError(expr.range, "неизвестное имя `" + ident.name + "`");
      return Type::Any;
    }
    if (ident.offset) checkOffset(*ident.offset, series);
    return Type::Num;
  }

  void checkOffset(const Offset& offset, bool allowed) {
    if (!allowed) {
      typeError(offset.range, "смещение допускается только у рядов (open, high, low, close, volume) и индикаторов");
      return;
    }
    if (const auto* number = std::get_if<std::int64_t>(&offset.value)) {
      if (offset.negative) {
        if (*number != 0) {
          report("E-LA01", Level::Error, offset.range,
                 "чтение будущего: отрицательное смещение -" + std::to_string(*number) + " берёт данные следующих баров");
        }
        return;
      }
      if (*number > kMaxBars) {
        typeError(offset.range, "смещение " + std::to_string(*number) + " больше " + std::to_string(kMaxBars) + " баров");
      }
      return;
    }
    const std::string& name = std::get<std::string>(offset.value);
    const ParamInfo* info = findParam(name);
    if (info == nullptr) {
      if (isBuiltinName(name)) {
        typeError(offset.range, "смещение может быть целым числом или параметром, а не `" + name + "`");
      } else {
        typeError(offset.range, "неизвестное имя `" + name + "`");
      }
      return;
    }
    if (offset.negative) {
      report("E-LA01", Level::Error, offset.range,
             "чтение будущего: отрицательное смещение -" + name + " берёт данные следующих баров");
      return;
    }
    checkIntegerParam(*info, offset.range, true);
  }

  Type checkCall(const Call& call, const Expr& expr) {
    const Signature* signature = findSignature(call.name);
    if (signature == nullptr) {
      typeError(expr.range, "неизвестная функция `" + call.name + "`");
      for (const ExprPtr& arg : call.args) checkExpr(*arg);
      return Type::Any;
    }
    if (call.args.size() != signature->args.size()) {
      typeError(expr.range, "функция `" + call.name + "` принимает аргументов: " +
                                std::to_string(signature->args.size()) + ", передано " +
                                std::to_string(call.args.size()));
      for (const ExprPtr& arg : call.args) checkExpr(*arg);
    } else {
      for (std::size_t i = 0; i < call.args.size(); ++i) {
        const Expr& arg = *call.args[i];
        if (signature->args[i] == ArgKind::Period) {
          checkPeriod(arg);
        } else if (checkExpr(arg) == Type::Bool) {
          typeError(arg.range, "аргумент функции `" + call.name + "` должен быть числом, а не логическим значением");
        }
      }
    }
    if (call.offset) checkOffset(*call.offset, signature->indicator);
    return signature->result;
  }

  // Период: целый литерал 1..5000 либо целочисленный параметр (dsl.md, «Функции»).
  void checkPeriod(const Expr& arg) {
    if (const auto* number = std::get_if<Number>(&arg.node)) {
      if (!isWhole(number->value)) {
        typeError(arg.range, "период должен быть целым числом, а не " + number->text);
      } else if (number->value < 1 || number->value > static_cast<double>(kMaxBars)) {
        typeError(arg.range, "период " + number->text + " вне пределов 1.." + std::to_string(kMaxBars));
      }
      return;
    }
    if (const auto* ident = std::get_if<Ident>(&arg.node); ident != nullptr && !ident->offset) {
      if (const ParamInfo* info = findParam(ident->name)) {
        checkIntegerParam(*info, arg.range, false);
        return;
      }
      if (!isBuiltinName(ident->name)) {
        typeError(arg.range, "неизвестное имя `" + ident->name + "`");
        return;
      }
    }
    typeError(arg.range, "период должен быть целым числом или целочисленным параметром");
    checkExpr(arg);
  }

  Type checkUnary(const Unary& unary, const Expr& expr) {
    const Type operand = checkExpr(*unary.operand);
    if (unary.op == '!') {
      if (operand == Type::Num) typeError(expr.range, "`not` применяется к логическому значению, а не к числу");
      return Type::Bool;
    }
    if (operand == Type::Bool) typeError(expr.range, "унарный минус применяется к числу, а не к логическому значению");
    return Type::Num;
  }

  Type checkBinary(const Binary& binary, const Expr& expr) {
    const Type left = checkExpr(*binary.left);
    const Type right = checkExpr(*binary.right);
    const std::string& op = binary.op;

    if (op == "and" || op == "or") {
      if (left == Type::Num || right == Type::Num) {
        typeError(expr.range, "`" + op + "` соединяет логические значения, а не числа");
      }
      return Type::Bool;
    }
    if (op == "==" || op == "!=") {
      if ((left == Type::Num && right == Type::Bool) || (left == Type::Bool && right == Type::Num)) {
        typeError(expr.range, "нельзя сравнивать число с логическим значением");
      } else {
        warnAbsoluteLevel(binary, expr);
      }
      return Type::Bool;
    }
    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
      if (left == Type::Bool || right == Type::Bool) {
        typeError(expr.range, "`" + op + "` сравнивает числа, а не логические значения");
      } else {
        warnAbsoluteLevel(binary, expr);
      }
      return Type::Bool;
    }
    // + - * /
    if (left == Type::Bool || right == Type::Bool) {
      typeError(expr.range, "арифметика `" + op + "` применяется к числам, а не к логическим значениям");
    }
    return Type::Num;
  }

  // ---- W-LA01 ----

  // `open`/`high`/`low`/`close` со смещением или без.
  static bool isPriceSeriesExpr(const Expr& expr) {
    const auto* ident = std::get_if<Ident>(&expr.node);
    return ident != nullptr && isPriceSeries(ident->name);
  }

  // Цена либо `sma`/`ema`/`highest`/`lowest` от цены.
  static bool isPriceLike(const Expr& expr) {
    if (isPriceSeriesExpr(expr)) return true;
    const auto* call = std::get_if<Call>(&expr.node);
    if (call == nullptr || call->args.empty()) return false;
    const std::string& name = call->name;
    return (name == "sma" || name == "ema" || name == "highest" || name == "lowest") &&
           isPriceSeriesExpr(*call->args[0]);
  }

  // Выражение только из чисел и параметров.
  bool isConstant(const Expr& expr) const {
    if (std::holds_alternative<Number>(expr.node)) return true;
    if (const auto* ident = std::get_if<Ident>(&expr.node)) return !ident->offset && findParam(ident->name) != nullptr;
    if (const auto* unary = std::get_if<Unary>(&expr.node)) return unary->op == '-' && isConstant(*unary->operand);
    if (const auto* binary = std::get_if<Binary>(&expr.node)) {
      const bool arithmetic = binary->op == "+" || binary->op == "-" || binary->op == "*" || binary->op == "/";
      return arithmetic && isConstant(*binary->left) && isConstant(*binary->right);
    }
    return false;
  }

  void warnAbsoluteLevel(const Binary& binary, const Expr& expr) {
    const bool absolute = (isPriceLike(*binary.left) && isConstant(*binary.right)) ||
                          (isConstant(*binary.left) && isPriceLike(*binary.right));
    if (absolute) {
      report("W-LA01", Level::Warning, expr.range,
             "цена сравнивается с абсолютным уровнем: такой уровень обычно подобран по известной истории");
    }
  }

  const Program& program_;
  std::map<std::string, ParamInfo> params_;  // упорядоченный контейнер: обход не зависит от адресов (INV-4)
  std::vector<ResolvedParam> resolved_;
  std::vector<bool> integerUse_;
  std::vector<Diagnostic> diagnostics_;
};

}  // namespace

CheckResult check(const Program& program) { return Checker(program).run(); }

Level overallLevel(const std::vector<Diagnostic>& diagnostics) {
  Level worst = Level::Ok;
  for (const Diagnostic& diagnostic : diagnostics) worst = std::max(worst, diagnostic.level);
  return worst;
}

}  // namespace core::dsl
