#include "core/dsl/interpreter.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "core/dsl/indicators.hpp"

namespace core::dsl {
namespace {

// ---- числа и деньги
// ---------------------------------------------------------------------

// Десятичная запись с 9 знаками (to_chars, fixed): точное значение двоичного
// числа, округлённое до 9 знаков без зависимости от локали и платформы (INV-4).
std::string fixed9(double value) {
  char buffer[512]; // 1e308 - 309 цифр, плюс знак, точка и 9 знаков
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                    std::chars_format::fixed, 9);
  if (result.ec != std::errc())
    throw std::invalid_argument("число не помещается в десятичную запись");
  return std::string(buffer, result.ptr);
}

// Число для подстановки в `because` (dsl.md, «Причина»): 2 знака, HALF_UP, без
// лишних нулей, запятая.
std::string formatNumber(double value) {
  try {
    std::string text = Decimal::parse(fixed9(value))
                           .round(2, RoundingMode::HALF_UP)
                           .toString();
    for (char &c : text) {
      if (c == '.')
        c = ',';
    }
    return text;
  } catch (const DecimalOverflow &) {
    return "н/д"; // число за пределами денежного типа: показать нечего
  } catch (const std::invalid_argument &) {
    return "н/д"; // не конечное число
  }
}

bool truthy(const Value &v) { return v.has_value() && *v != 0.0; }
Value fromBool(bool b) { return b ? 1.0 : 0.0; }
Value finiteOrNa(double v) {
  return std::isfinite(v) ? Value(v) : std::nullopt;
}

constexpr std::int64_t kMaxBars = 5000; // как в checker: смещение и период

// ---- ряды
// -------------------------------------------------------------------------------

// Кольцевой буфер значений: at(k) - значение k баров назад, na, если истории
// меньше.
class Ring {
public:
  explicit Ring(std::size_t capacity = 1)
      : buffer_(capacity < 1 ? 1 : capacity) {}
  void push(const Value &v) {
    head_ = (head_ + 1) % buffer_.size();
    buffer_[head_] = v;
    if (count_ < buffer_.size())
      ++count_;
  }
  Value at(std::size_t k) const {
    if (k >= count_)
      return std::nullopt;
    return buffer_[(head_ + buffer_.size() - k) % buffer_.size()];
  }

private:
  std::vector<Value> buffer_;
  std::size_t head_ = 0, count_ = 0;
};

// ---- глубокая копия программы
// ------------------------------------------------------------ Program не
// копируется (в дереве unique_ptr), а переопределения параметров нужно записать
// в копию.

ExprPtr cloneExpr(const Expr &expr);

ExprPtr cloneExpr(const Expr &expr) {
  auto copy = std::make_unique<Expr>();
  copy->range = expr.range;
  copy->height = expr.height;
  struct Visitor {
    Expr &out;
    void operator()(const Number &n) const { out.node = n; }
    void operator()(const Bool &b) const { out.node = b; }
    void operator()(const Na &n) const { out.node = n; }
    void operator()(const Ident &i) const { out.node = i; }
    void operator()(const Call &c) const {
      Call call;
      call.name = c.name;
      call.offset = c.offset;
      for (const ExprPtr &arg : c.args)
        call.args.push_back(cloneExpr(*arg));
      out.node = std::move(call);
    }
    void operator()(const Unary &u) const {
      out.node = Unary{u.op, cloneExpr(*u.operand)};
    }
    void operator()(const Binary &b) const {
      out.node = Binary{b.op, cloneExpr(*b.left), cloneExpr(*b.right)};
    }
  };
  std::visit(Visitor{*copy}, expr.node);
  return copy;
}

Program cloneProgram(const Program &program) {
  Program copy;
  copy.params = program.params;
  for (const Rule &rule : program.rules) {
    Rule r;
    r.name = rule.name;
    r.when = cloneExpr(*rule.when);
    r.action.kind = rule.action.kind;
    r.action.sizeKind = rule.action.sizeKind;
    if (rule.action.size)
      r.action.size = cloneExpr(*rule.action.size);
    r.action.orderKind = rule.action.orderKind;
    if (rule.action.price)
      r.action.price = cloneExpr(*rule.action.price);
    r.action.day = rule.action.day;
    r.action.range = rule.action.range;
    r.because.range = rule.because.range;
    for (const auto &part : rule.because.parts) {
      if (const auto *text = std::get_if<std::string>(&part)) {
        r.because.parts.emplace_back(*text);
      } else {
        r.because.parts.emplace_back(cloneExpr(*std::get<ExprPtr>(part)));
      }
    }
    r.range = rule.range;
    copy.rules.push_back(std::move(r));
  }
  return copy;
}

// ---- скомпилированное выражение
// -----------------------------------------------------------

enum class Kind { Const, Na, Series, State, Indicator, Neg, Not, Binary, Func };
enum class SeriesName { Open, High, Low, Close, Volume };
constexpr int kSeriesCount = 5;
enum class StateName { Position, EntryPrice, Cash, Equity, BarsInPosition };
enum class BinOp { Or, And, Lt, Le, Gt, Ge, Eq, Ne, Add, Sub, Mul, Div };
enum class FuncOp { Abs, Sqrt, Min, Max };

using Idx = int;

struct CNode {
  Kind kind = Kind::Const;
  double value = 0; // Const
  SeriesName series = SeriesName::Close;
  StateName state = StateName::Position;
  BinOp bin = BinOp::Add;
  FuncOp func = FuncOp::Abs;
  int indicator = -1; // Indicator: номер в списке индикаторов
  int offset = 0;     // Series и Indicator: смещение назад
  Idx a = -1, b = -1; // операнды
  bool isBool =
      false; // логическое по типу: подстановка в because печатает «да» / «нет»
  bool needsState =
      false; // зависит от состояния портфеля (прямо или через индикатор)
};

// Индикатор с входами. Одинаковые вызовы (одна функция, одинаковые аргументы) -
// один экземпляр.
struct IndicatorSpec {
  std::string name;
  std::vector<std::int64_t> intArgs;
  std::vector<Idx> inputs;
  bool needsState = false;
  int maxOffset =
      0; // сколько баров истории результата нужно (смещения у вызова)
  bool isBool = false;
};

struct CPart {
  std::string text;
  Idx expr = -1; // -1 - текстовая часть
};

struct CRule {
  std::string name;
  Idx when = -1;
  Action::Kind kind = Action::Kind::Cancel;
  SizeKind sizeKind = SizeKind::Lots;
  Idx size = -1;
  OrderKind orderKind = OrderKind::Market;
  Idx price = -1;
  TimeInForce tif = TimeInForce::Gtc;
  std::vector<CPart> because;
};

// Состояние одной бумаги.
struct InstrumentState {
  std::vector<Ring> series; // open high low close volume
  std::vector<std::unique_ptr<Indicator>> indicators;
  std::vector<Ring> history; // результаты индикаторов по барам
  std::optional<std::int64_t> barStep; // шаг последнего onBar
  std::optional<std::int64_t> evaluatedStep; // шаг последнего evaluate
};

} // namespace

// ---- Impl
// ----------------------------------------------------------------------------------

struct StrategyInstance::Impl {
  Program program; // своя копия: с переопределёнными параметрами, проверенная
                   // заново
  std::map<std::string, double> paramValues;
  std::vector<CNode> nodes;
  std::vector<IndicatorSpec> specs;
  std::map<std::string, int> specByKey; // упорядоченный контейнер: порядок не
                                        // зависит от адресов (INV-4)
  std::vector<CRule> rules;
  int maxSeriesOffset = 0;
  std::vector<InstrumentState> instruments;

  // ---- компиляция ----

  Idx add(CNode node) {
    nodes.push_back(node);
    return static_cast<Idx>(nodes.size() - 1);
  }

  // Целое значение смещения или периода: литерал или параметр (проверено
  // check()).
  std::int64_t wholeValue(double v, const char *what) const {
    if (!(std::floor(v) == v) || v < 0 || v > static_cast<double>(kMaxBars)) {
      throw std::invalid_argument(std::string("недопустимое значение для ") +
                                  what);
    }
    return static_cast<std::int64_t>(v);
  }

  double paramValue(const std::string &name) const {
    const auto found = paramValues.find(name);
    if (found == paramValues.end())
      throw std::invalid_argument("неизвестное имя `" + name + "`");
    return found->second;
  }

  int resolveOffset(const Offset &offset) const {
    if (offset.negative) {
      // Невозможно после check(): E-LA01. Защита от чтения будущего (INV-1).
      if (const auto *n = std::get_if<std::int64_t>(&offset.value);
          n != nullptr && *n == 0)
        return 0;
      throw std::invalid_argument("отрицательное смещение: чтение будущего");
    }
    if (const auto *n = std::get_if<std::int64_t>(&offset.value)) {
      return static_cast<int>(wholeValue(static_cast<double>(*n), "смещения"));
    }
    return static_cast<int>(wholeValue(
        paramValue(std::get<std::string>(offset.value)), "смещения"));
  }

  static bool seriesByName(const std::string &name, SeriesName &out) {
    if (name == "open")
      out = SeriesName::Open;
    else if (name == "high")
      out = SeriesName::High;
    else if (name == "low")
      out = SeriesName::Low;
    else if (name == "close")
      out = SeriesName::Close;
    else if (name == "volume")
      out = SeriesName::Volume;
    else
      return false;
    return true;
  }

  static bool stateByName(const std::string &name, StateName &out) {
    if (name == "position")
      out = StateName::Position;
    else if (name == "entry_price")
      out = StateName::EntryPrice;
    else if (name == "cash")
      out = StateName::Cash;
    else if (name == "equity")
      out = StateName::Equity;
    else if (name == "bars_in_position")
      out = StateName::BarsInPosition;
    else
      return false;
    return true;
  }

  Idx seriesNode(SeriesName series, int offset) {
    CNode node;
    node.kind = Kind::Series;
    node.series = series;
    node.offset = offset;
    if (offset > maxSeriesOffset)
      maxSeriesOffset = offset;
    return add(node);
  }

  Idx compile(const Expr &expr) {
    struct Visitor {
      StrategyInstance::Impl &self;
      Idx operator()(const Number &n) const {
        CNode node;
        node.value = n.value;
        return self.add(node);
      }
      Idx operator()(const Bool &b) const {
        CNode node;
        node.value = b.value ? 1.0 : 0.0;
        node.isBool = true;
        return self.add(node);
      }
      Idx operator()(const Na &) const {
        CNode node;
        node.kind = Kind::Na;
        return self.add(node);
      }
      Idx operator()(const Ident &ident) const {
        return self.compileIdent(ident);
      }
      Idx operator()(const Call &call) const { return self.compileCall(call); }
      Idx operator()(const Unary &unary) const {
        CNode node;
        node.a = self.compile(*unary.operand);
        node.kind = unary.op == '!' ? Kind::Not : Kind::Neg;
        node.isBool = unary.op == '!';
        node.needsState =
            self.nodes[static_cast<std::size_t>(node.a)].needsState;
        return self.add(node);
      }
      Idx operator()(const Binary &binary) const {
        return self.compileBinary(binary);
      }
    };
    return std::visit(Visitor{*this}, expr.node);
  }

  Idx compileIdent(const Ident &ident) {
    SeriesName series;
    StateName state;
    if (seriesByName(ident.name, series)) {
      return seriesNode(series,
                        ident.offset ? resolveOffset(*ident.offset) : 0);
    }
    if (stateByName(ident.name, state)) {
      if (ident.offset)
        throw std::invalid_argument(
            "смещение допускается только у рядов и индикаторов");
      CNode node;
      node.kind = Kind::State;
      node.state = state;
      node.needsState = true;
      return add(node);
    }
    if (ident.offset)
      throw std::invalid_argument(
          "смещение допускается только у рядов и индикаторов");
    CNode node;
    node.value = paramValue(ident.name);
    return add(node);
  }

  Idx compileBinary(const Binary &binary) {
    static const std::pair<const char *, BinOp> table[] = {
        {"or", BinOp::Or}, {"and", BinOp::And}, {"<", BinOp::Lt},
        {"<=", BinOp::Le}, {">", BinOp::Gt},    {">=", BinOp::Ge},
        {"==", BinOp::Eq}, {"!=", BinOp::Ne},   {"+", BinOp::Add},
        {"-", BinOp::Sub}, {"*", BinOp::Mul},   {"/", BinOp::Div}};
    CNode node;
    node.kind = Kind::Binary;
    bool found = false;
    for (const auto &[text, op] : table) {
      if (binary.op == text) {
        node.bin = op;
        found = true;
      }
    }
    if (!found)
      throw std::invalid_argument("неизвестный оператор `" + binary.op + "`");
    node.isBool = node.bin <= BinOp::Ne;
    node.a = compile(*binary.left);
    node.b = compile(*binary.right);
    node.needsState = nodes[static_cast<std::size_t>(node.a)].needsState ||
                      nodes[static_cast<std::size_t>(node.b)].needsState;
    return add(node);
  }

  // Период индикатора: целый литерал или параметр.
  std::int64_t compilePeriod(const Expr &arg) const {
    if (const auto *number = std::get_if<Number>(&arg.node))
      return wholeValue(number->value, "периода");
    if (const auto *ident = std::get_if<Ident>(&arg.node);
        ident != nullptr && !ident->offset) {
      return wholeValue(paramValue(ident->name), "периода");
    }
    throw std::invalid_argument(
        "период должен быть целым числом или целочисленным параметром");
  }

  Idx compileCall(const Call &call) {
    const std::string &name = call.name;
    if (name == "abs" || name == "sqrt" || name == "min" || name == "max") {
      if (call.offset)
        throw std::invalid_argument(
            "смещение допускается только у рядов и индикаторов");
      CNode node;
      node.kind = Kind::Func;
      node.func = name == "abs"    ? FuncOp::Abs
                  : name == "sqrt" ? FuncOp::Sqrt
                  : name == "min"  ? FuncOp::Min
                                   : FuncOp::Max;
      const std::size_t arity =
          (node.func == FuncOp::Min || node.func == FuncOp::Max) ? 2 : 1;
      if (call.args.size() != arity)
        throw std::invalid_argument("неверное число аргументов `" + name + "`");
      node.a = compile(*call.args[0]);
      node.needsState = nodes[static_cast<std::size_t>(node.a)].needsState;
      if (arity == 2) {
        node.b = compile(*call.args[1]);
        node.needsState = node.needsState ||
                          nodes[static_cast<std::size_t>(node.b)].needsState;
      }
      return add(node);
    }

    // Индикаторы считает indicators.cpp; интерпретатор только подаёт им входы
    // на каждом баре, `na` - как есть. Для change(x, n) это значит: x - n-е
    // значение назад среди не-na значений x (окно из n + 1 не-na значений), а
    // не значение n баров назад; пока окно не заполнено - `na`.
    IndicatorSpec spec;
    spec.name = name;
    if (name == "atr") {
      if (call.args.size() != 1)
        throw std::invalid_argument("неверное число аргументов `atr`");
      spec.intArgs = {compilePeriod(*call.args[0])};
      spec.inputs = {seriesNode(SeriesName::High, 0),
                     seriesNode(SeriesName::Low, 0),
                     seriesNode(SeriesName::Close, 0)};
    } else if (name == "crossover" || name == "crossunder") {
      if (call.args.size() != 2)
        throw std::invalid_argument("неверное число аргументов `" + name + "`");
      spec.inputs = {compile(*call.args[0]), compile(*call.args[1])};
      spec.isBool = true;
    } else {
      // sma ema rsi stdev highest lowest change: (x, n)
      if (call.args.size() != 2)
        throw std::invalid_argument("неверное число аргументов `" + name + "`");
      spec.inputs = {compile(*call.args[0])};
      spec.intArgs = {compilePeriod(*call.args[1])};
    }
    for (Idx input : spec.inputs)
      spec.needsState =
          spec.needsState || nodes[static_cast<std::size_t>(input)].needsState;

    const std::string key = specKey(spec);
    int index;
    if (const auto found = specByKey.find(key); found != specByKey.end()) {
      index = found->second;
    } else {
      index = static_cast<int>(specs.size());
      specs.push_back(spec);
      specByKey.emplace(key, index);
    }
    const int offset = call.offset ? resolveOffset(*call.offset) : 0;
    IndicatorSpec &stored = specs[static_cast<std::size_t>(index)];
    if (offset > stored.maxOffset)
      stored.maxOffset = offset;

    CNode node;
    node.kind = Kind::Indicator;
    node.indicator = index;
    node.offset = offset;
    node.isBool = stored.isBool;
    node.needsState = stored.needsState;
    return add(node);
  }

  // Каноническая запись дерева: ключ дедупликации индикаторов. Параметры уже
  // подставлены значениями, поэтому `sma(close, fast)` при fast = 20 и
  // `sma(close, 20)` - один индикатор: состояние индикатора - чистая функция
  // потока входов.
  std::string keyOf(Idx index) const {
    const CNode &node = nodes[static_cast<std::size_t>(index)];
    switch (node.kind) {
    case Kind::Const: {
      char buffer[64];
      const auto r = std::to_chars(buffer, buffer + sizeof(buffer), node.value);
      return "c" + std::string(buffer, r.ptr);
    }
    case Kind::Na:
      return "na";
    case Kind::Series:
      return "s" + std::to_string(static_cast<int>(node.series)) + "[" +
             std::to_string(node.offset) + "]";
    case Kind::State:
      return "p" + std::to_string(static_cast<int>(node.state));
    case Kind::Indicator:
      return "i" + std::to_string(node.indicator) + "[" +
             std::to_string(node.offset) + "]";
    case Kind::Neg:
      return "(neg " + keyOf(node.a) + ")";
    case Kind::Not:
      return "(not " + keyOf(node.a) + ")";
    case Kind::Binary:
      return "(b" + std::to_string(static_cast<int>(node.bin)) + " " +
             keyOf(node.a) + " " + keyOf(node.b) + ")";
    case Kind::Func:
      return "(f" + std::to_string(static_cast<int>(node.func)) + " " +
             keyOf(node.a) + (node.b >= 0 ? " " + keyOf(node.b) : "") + ")";
    }
    return "";
  }

  std::string specKey(const IndicatorSpec &spec) const {
    std::string key = spec.name;
    for (std::int64_t n : spec.intArgs)
      key += ":" + std::to_string(n);
    for (Idx input : spec.inputs)
      key += "|" + keyOf(input);
    return key;
  }

  void compileRules() {
    for (const Rule &rule : program.rules) {
      CRule compiled;
      compiled.name = rule.name;
      compiled.when = compile(*rule.when);
      compiled.kind = rule.action.kind;
      compiled.sizeKind = rule.action.sizeKind;
      if (rule.action.size)
        compiled.size = compile(*rule.action.size);
      compiled.orderKind = rule.action.orderKind;
      if (rule.action.price)
        compiled.price = compile(*rule.action.price);
      compiled.tif =
          rule.action.day.value_or(false) ? TimeInForce::Day : TimeInForce::Gtc;
      for (const auto &part : rule.because.parts) {
        CPart p;
        if (const auto *text = std::get_if<std::string>(&part)) {
          p.text = *text;
        } else {
          p.expr = compile(*std::get<ExprPtr>(part));
        }
        compiled.because.push_back(std::move(p));
      }
      rules.push_back(std::move(compiled));
    }
  }

  void makeInstruments(std::size_t count) {
    instruments.resize(count);
    for (InstrumentState &inst : instruments) {
      inst.series.assign(kSeriesCount,
                         Ring(static_cast<std::size_t>(maxSeriesOffset) + 1));
      for (const IndicatorSpec &spec : specs) {
        inst.indicators.push_back(makeIndicator(spec.name, spec.intArgs));
        inst.history.emplace_back(static_cast<std::size_t>(spec.maxOffset) + 1);
      }
    }
  }

  // ---- вычисление ----

  struct Context {
    const InstrumentState &inst;
    const StateView *state; // null при onBar: узлы без зависимости от состояния
                            // его не читают
  };

  Value stateValue(StateName name, const StateView &s) const {
    switch (name) {
    case StateName::Position:
      return static_cast<double>(s.positionLots);
    case StateName::EntryPrice:
      return s.entryPrice;
    case StateName::Cash:
      return s.cash;
    case StateName::Equity:
      return s.equity;
    case StateName::BarsInPosition:
      if (!s.barsInPosition)
        return std::nullopt;
      return static_cast<double>(*s.barsInPosition);
    }
    return std::nullopt;
  }

  Value eval(Idx index, const Context &ctx) const {
    const CNode &node = nodes[static_cast<std::size_t>(index)];
    switch (node.kind) {
    case Kind::Const:
      return node.value;
    case Kind::Na:
      return std::nullopt;
    case Kind::Series:
      return ctx.inst.series[static_cast<std::size_t>(node.series)].at(
          static_cast<std::size_t>(node.offset));
    case Kind::State:
      if (ctx.state == nullptr)
        throw std::logic_error("состояние портфеля недоступно при onBar");
      {
        const Value v = stateValue(node.state, *ctx.state);
        return v ? finiteOrNa(*v)
                 : v; // состояние приходит от вызывающего: не конечное - na
      }
    case Kind::Indicator: { // выход индикатора мог выйти за double (сумма окна
                            // переполнилась): это na, а не inf/NaN
      const Value v =
          ctx.inst.history[static_cast<std::size_t>(node.indicator)].at(
              static_cast<std::size_t>(node.offset));
      return v ? finiteOrNa(*v) : v;
    }
    case Kind::Neg: {
      const Value v = eval(node.a, ctx);
      if (!v)
        return std::nullopt;
      return -*v;
    }
    case Kind::Not:
      return fromBool(!truthy(eval(node.a, ctx))); // `na` в логике - false
    case Kind::Binary:
      return evalBinary(node, ctx);
    case Kind::Func:
      return evalFunc(node, ctx);
    }
    return std::nullopt;
  }

  Value evalBinary(const CNode &node, const Context &ctx) const {
    // and / or: правый операнд не вычисляется, если результат ясен. Индикаторы
    // это не затрагивает: они обновляются отдельно, до вычисления правил.
    if (node.bin == BinOp::Or)
      return fromBool(truthy(eval(node.a, ctx)) || truthy(eval(node.b, ctx)));
    if (node.bin == BinOp::And)
      return fromBool(truthy(eval(node.a, ctx)) && truthy(eval(node.b, ctx)));

    const Value left = eval(node.a, ctx);
    const Value right = eval(node.b, ctx);
    switch (node.bin) {
    case BinOp::Lt:
    case BinOp::Le:
    case BinOp::Gt:
    case BinOp::Ge:
    case BinOp::Eq:
    case BinOp::Ne: {
      if (!left || !right)
        return fromBool(false); // сравнение с na - false
      switch (node.bin) {
      case BinOp::Lt:
        return fromBool(*left < *right);
      case BinOp::Le:
        return fromBool(*left <= *right);
      case BinOp::Gt:
        return fromBool(*left > *right);
      case BinOp::Ge:
        return fromBool(*left >= *right);
      case BinOp::Eq:
        return fromBool(*left == *right);
      default:
        return fromBool(*left != *right);
      }
    }
    default:
      break;
    }
    if (!left || !right)
      return std::nullopt; // арифметика с na - na
    switch (node.bin) {
    case BinOp::Add:
      return finiteOrNa(*left + *right);
    case BinOp::Sub:
      return finiteOrNa(*left - *right);
    case BinOp::Mul:
      return finiteOrNa(*left * *right);
    default: // Div
      if (*right == 0.0)
        return std::nullopt; // деление на 0 - na
      return finiteOrNa(*left / *right);
    }
  }

  Value evalFunc(const CNode &node, const Context &ctx) const {
    const Value a = eval(node.a, ctx);
    if (node.func == FuncOp::Abs)
      return a ? Value(std::fabs(*a)) : std::nullopt;
    if (node.func == FuncOp::Sqrt) {
      if (!a || *a < 0)
        return std::nullopt;
      return std::sqrt(*a);
    }
    const Value b = eval(node.b, ctx);
    if (!a || !b)
      return std::nullopt;
    if (node.func == FuncOp::Min)
      return *a < *b ? *a : *b;
    return *a > *b ? *a : *b;
  }

  // Обновить индикаторы бумаги, у которых зависимость от состояния равна
  // stateful. Порядок списка - порядок создания, входы создаются раньше самого
  // индикатора: обход топологический.
  void updateIndicators(InstrumentState &inst, bool stateful,
                        const StateView *state) const {
    const Context ctx{inst, state};
    std::vector<Value> inputs;
    for (std::size_t i = 0; i < specs.size(); ++i) {
      if (specs[i].needsState != stateful)
        continue;
      inputs.clear();
      for (Idx input : specs[i].inputs)
        inputs.push_back(eval(input, ctx));
      inst.history[i].push(inst.indicators[i]->update(inputs));
    }
  }

  // ---- действия ----

  static std::optional<std::int64_t> toLots(double floored) {
    if (!std::isfinite(floored) || floored < 1.0 || floored >= 9.0e18)
      return std::nullopt;
    return static_cast<std::int64_t>(floored);
  }

  std::optional<std::int64_t> computeLots(const CRule &rule, const Context &ctx,
                                          double close,
                                          std::int64_t lotSize) const {
    const StateView &state = *ctx.state;
    if (rule.sizeKind == SizeKind::All)
      return toLots(static_cast<double>(state.positionLots));
    const Value n = eval(rule.size, ctx);
    if (!n)
      return std::nullopt;
    switch (rule.sizeKind) {
    case SizeKind::Lots:
      return toLots(std::floor(*n));
    case SizeKind::PercentEquity:
    case SizeKind::PercentCash: {
      const double base =
          rule.sizeKind == SizeKind::PercentEquity ? state.equity : state.cash;
      const double lotCost = close * static_cast<double>(lotSize);
      if (!(lotCost > 0))
        return std::nullopt;
      return toLots(std::floor(base * *n / 100 / lotCost));
    }
    case SizeKind::PercentPosition:
      return toLots(
          std::floor(static_cast<double>(state.positionLots) * *n / 100));
    case SizeKind::All:
      break;
    }
    return std::nullopt;
  }

  std::string renderBecause(const CRule &rule, const Context &ctx) const {
    std::string out;
    for (const CPart &part : rule.because) {
      if (part.expr < 0) {
        out += part.text;
        continue;
      }
      const Value v = eval(part.expr, ctx);
      if (!v) {
        out += "н/д";
      } else if (nodes[static_cast<std::size_t>(part.expr)].isBool) {
        out += *v != 0.0 ? "да" : "нет";
      } else {
        out += formatNumber(*v);
      }
    }
    return out;
  }

  std::optional<Intent> makeIntent(const CRule &rule, int instrument,
                                   const Context &ctx, double close,
                                   std::int64_t lotSize,
                                   const Decimal &priceStep) const {
    Intent intent;
    intent.instrument = instrument;
    intent.ruleName = rule.name;
    intent.order.instrument = instrument;
    if (rule.kind == Action::Kind::Cancel) {
      intent.cancelAll = true;
      intent.because = renderBecause(rule, ctx);
      return intent;
    }

    const std::optional<std::int64_t> lots =
        computeLots(rule, ctx, close, lotSize);
    if (!lots)
      return std::nullopt;
    intent.order.side = rule.kind == Action::Kind::Buy ? Side::Buy : Side::Sell;
    intent.order.lots = *lots;
    intent.order.tif = rule.tif;

    if (rule.orderKind == OrderKind::Market) {
      intent.order.type = OrderType::Market;
    } else {
      const Value p = eval(rule.price, ctx);
      if (!p || !std::isfinite(*p) || !(*p > 0))
        return std::nullopt;
      Decimal price;
      try {
        price = toDecimalPrice(*p, priceStep);
      } catch (const DecimalOverflow &) {
        return std::nullopt; // цена за пределами денежного типа - заявку не
                             // создаём
      }
      if (price.sign() <= 0)
        return std::nullopt; // округлилась до 0
      switch (rule.orderKind) {
      case OrderKind::Limit:
        intent.order.type = OrderType::Limit;
        intent.order.limitPrice = price;
        break;
      case OrderKind::Stop:
        intent.order.type = OrderType::Stop;
        intent.order.stopPrice = price;
        break;
      default: // TakeProfit: цена - в limitPrice (exec/order.hpp)
        intent.order.type = OrderType::TakeProfit;
        intent.order.limitPrice = price;
        break;
      }
    }
    intent.because = renderBecause(rule, ctx);
    return intent;
  }
};

// ---- публичный интерфейс
// -----------------------------------------------------------------------

Decimal toDecimalPrice(double value, const Decimal &priceStep) {
  if (!std::isfinite(value))
    throw std::invalid_argument("цена не является конечным числом");
  return Decimal::parse(fixed9(value))
      .roundToStep(priceStep, RoundingMode::HALF_UP);
}

StrategyInstance::StrategyInstance(const Program &program,
                                   const std::vector<ResolvedParam> &params,
                                   const ParamValues &overrides,
                                   std::size_t instrumentCount)
    : impl_(std::make_unique<Impl>()) {
  if (instrumentCount < 1)
    throw std::invalid_argument("в сессии нет бумаг");
  impl_->program = cloneProgram(program);

  for (const auto &[name, value] : overrides) {
    const ResolvedParam *declared = nullptr;
    for (const ResolvedParam &p : params) {
      if (p.name == name)
        declared = &p;
    }
    if (declared == nullptr)
      throw std::invalid_argument("неизвестный параметр `" + name + "`");
    if (!std::isfinite(value))
      throw std::invalid_argument("параметр `" + name + "`: значение не число");
    if ((declared->min && value < *declared->min) ||
        (declared->max && value > *declared->max)) {
      throw std::invalid_argument("параметр `" + name +
                                  "`: значение вне диапазона");
    }
    for (ParamDecl &decl : impl_->program.params) {
      if (decl.name == name) {
        decl.value = value;
        break; // первое объявление: как в check()
      }
    }
  }

  // Программа с новыми значениями проверяется заново (INV-1): `k = -1` в
  // `close[k]`, `n = 0` или `n = 100000` в `sma(close, n)` дают ошибку, и
  // экземпляр не создаётся. Проверка идёт и без переопределений: интерпретатор
  // не исполняет программу с ошибками.
  const CheckResult checked = check(impl_->program);
  if (checked.level == Level::Error) {
    for (const Diagnostic &d : checked.diagnostics) {
      if (d.level == Level::Error) {
        throw std::invalid_argument("программа не проходит проверку (" +
                                    d.code + "): " + d.message);
      }
    }
  }
  for (const ResolvedParam &p : checked.params)
    impl_->paramValues.emplace(p.name, p.value);

  impl_->compileRules();
  impl_->makeInstruments(instrumentCount);
}

StrategyInstance::~StrategyInstance() = default;
StrategyInstance::StrategyInstance(StrategyInstance &&) noexcept = default;
StrategyInstance &
StrategyInstance::operator=(StrategyInstance &&) noexcept = default;

void StrategyInstance::onBar(int instrument, const StepBar &bar) {
  if (!impl_)
    throw std::logic_error("onBar: экземпляр перемещён");
  if (instrument < 0 ||
      static_cast<std::size_t>(instrument) >= impl_->instruments.size()) {
    throw std::out_of_range("onBar: бумаги с таким номером нет в сессии");
  }
  if (bar.instrument != instrument)
    throw std::invalid_argument("onBar: бар другой бумаги");
  InstrumentState &inst =
      impl_->instruments[static_cast<std::size_t>(instrument)];
  if (inst.barStep && bar.step <= *inst.barStep) {
    throw std::invalid_argument("onBar: шаг бара должен возрастать");
  }
  if (inst.barStep && inst.evaluatedStep != inst.barStep) {
    // Бар без evaluate: индикаторы от состояния портфеля потеряли бы отсчёт.
    // Контракт с прогоном (задача 21).
    throw std::logic_error("onBar: предыдущий бар не вычислен через evaluate");
  }

  inst.series[static_cast<std::size_t>(SeriesName::Open)].push(
      bar.open.toDouble());
  inst.series[static_cast<std::size_t>(SeriesName::High)].push(
      bar.high.toDouble());
  inst.series[static_cast<std::size_t>(SeriesName::Low)].push(
      bar.low.toDouble());
  inst.series[static_cast<std::size_t>(SeriesName::Close)].push(
      bar.close.toDouble());
  inst.series[static_cast<std::size_t>(SeriesName::Volume)].push(
      static_cast<double>(bar.volumeLots));
  inst.barStep = bar.step;
  impl_->updateIndicators(inst, false, nullptr);
}

std::vector<Intent> StrategyInstance::evaluate(int instrument,
                                               const StepBar &bar,
                                               std::int64_t lotSize,
                                               const Decimal &priceStep,
                                               const StateView &state) {
  if (!impl_)
    throw std::logic_error("evaluate: экземпляр перемещён");
  if (instrument < 0 ||
      static_cast<std::size_t>(instrument) >= impl_->instruments.size()) {
    throw std::out_of_range("evaluate: бумаги с таким номером нет в сессии");
  }
  if (lotSize < 1)
    throw std::invalid_argument(
        "evaluate: размер лота должен быть не меньше 1");
  if (priceStep.sign() <= 0)
    throw std::invalid_argument("evaluate: шаг цены должен быть положительным");
  InstrumentState &inst =
      impl_->instruments[static_cast<std::size_t>(instrument)];
  if (bar.instrument != instrument || !inst.barStep ||
      *inst.barStep != bar.step) {
    throw std::logic_error(
        "evaluate: бар не совпадает с последним, переданным в onBar");
  }
  if (inst.evaluatedStep && *inst.evaluatedStep == bar.step) {
    throw std::logic_error("evaluate: бар уже вычислен");
  }
  inst.evaluatedStep = bar.step;

  impl_->updateIndicators(inst, true, &state);

  const Impl::Context ctx{inst, &state};
  const double close = bar.close.toDouble();
  std::vector<Intent> intents;
  for (const CRule &rule : impl_->rules) {
    if (!truthy(impl_->eval(rule.when, ctx)))
      continue;
    if (auto intent = impl_->makeIntent(rule, instrument, ctx, close, lotSize,
                                        priceStep)) {
      intents.push_back(std::move(*intent));
    }
  }
  return intents;
}

} // namespace core::dsl
