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
std::string fixed9(double Value) {
  char Buffer[512]; // 1e308 - 309 цифр, плюс знак, точка и 9 знаков
  const auto Result = std::to_chars(Buffer, Buffer + sizeof(Buffer), Value,
                                    std::chars_format::fixed, 9);
  if (Result.ec != std::errc())
    throw std::invalid_argument("число не помещается в десятичную запись");
  return std::string(Buffer, Result.ptr);
}

// Число для подстановки в `because` (dsl.md, «Причина»): 2 знака, HALF_UP, без
// лишних нулей, запятая.
std::string formatNumber(double Value) {
  try {
    std::string Text =
        Decimal::parse(fixed9(Value)).round(2, RoundingMode::HalfUp).toString();
    for (char &C : Text) {
      if (C == '.')
        C = ',';
    }
    return Text;
  } catch (const DecimalOverflow &) {
    return "н/д"; // число за пределами денежного типа: показать нечего
  } catch (const std::invalid_argument &) {
    return "н/д"; // не конечное число
  }
}

bool truthy(const Value &V) { return V.has_value() && *V != 0.0; }
Value fromBool(bool B) { return B ? 1.0 : 0.0; }
Value finiteOrNa(double V) {
  return std::isfinite(V) ? Value(V) : std::nullopt;
}

constexpr std::int64_t kMaxBars = 5000; // как в checker: смещение и период

// ---- ряды
// -------------------------------------------------------------------------------

// Кольцевой буфер значений: at(k) - значение k баров назад, na, если истории
// меньше.
class Ring {
public:
  explicit Ring(std::size_t Capacity = 1)
      : Buffer(Capacity < 1 ? 1 : Capacity) {}
  void push(const Value &V) {
    Head = (Head + 1) % Buffer.size();
    Buffer[Head] = V;
    if (Count < Buffer.size())
      ++Count;
  }
  Value at(std::size_t K) const {
    if (K >= Count)
      return std::nullopt;
    return Buffer[(Head + Buffer.size() - K) % Buffer.size()];
  }

private:
  std::vector<Value> Buffer;
  std::size_t Head = 0, Count = 0;
};

// ---- глубокая копия программы
// ------------------------------------------------------------ Program не
// копируется (в дереве unique_ptr), а переопределения параметров нужно записать
// в копию.

ExprPtr cloneExpr(const Expr &E);

ExprPtr cloneExpr(const Expr &E) {
  auto Copy = std::make_unique<Expr>();
  Copy->Range = E.Range;
  Copy->Height = E.Height;
  struct Visitor {
    Expr &Out;
    void operator()(const Number &N) const { Out.Node = N; }
    void operator()(const Bool &B) const { Out.Node = B; }
    void operator()(const Na &N) const { Out.Node = N; }
    void operator()(const Ident &I) const { Out.Node = I; }
    void operator()(const Call &C) const {
      Call Call;
      Call.Name = C.Name;
      Call.BarOffset = C.BarOffset;
      for (const ExprPtr &Arg : C.Args)
        Call.Args.push_back(cloneExpr(*Arg));
      Out.Node = std::move(Call);
    }
    void operator()(const Unary &U) const {
      Out.Node = Unary{U.Op, cloneExpr(*U.Operand)};
    }
    void operator()(const Binary &B) const {
      Out.Node = Binary{B.Op, cloneExpr(*B.Left), cloneExpr(*B.Right)};
    }
  };
  std::visit(Visitor{*Copy}, E.Node);
  return Copy;
}

Program cloneProgram(const Program &Source) {
  Program Copy;
  Copy.Params = Source.Params;
  for (const Rule &Src : Source.Rules) {
    Rule R;
    R.Name = Src.Name;
    R.When = cloneExpr(*Src.When);
    R.Then.ActionKind = Src.Then.ActionKind;
    R.Then.SizeUnit = Src.Then.SizeUnit;
    if (Src.Then.Size)
      R.Then.Size = cloneExpr(*Src.Then.Size);
    R.Then.OrderMode = Src.Then.OrderMode;
    if (Src.Then.Price)
      R.Then.Price = cloneExpr(*Src.Then.Price);
    R.Then.Day = Src.Then.Day;
    R.Then.Range = Src.Then.Range;
    R.Because.Range = Src.Because.Range;
    for (const auto &Part : Src.Because.Parts) {
      if (const auto *Text = std::get_if<std::string>(&Part)) {
        R.Because.Parts.emplace_back(*Text);
      } else {
        R.Because.Parts.emplace_back(cloneExpr(*std::get<ExprPtr>(Part)));
      }
    }
    R.Range = Src.Range;
    Copy.Rules.push_back(std::move(R));
  }
  return Copy;
}

// ---- скомпилированное выражение
// -----------------------------------------------------------

enum class NodeKind {
  Const,
  Na,
  Series,
  State,
  Indicator,
  Neg,
  Not,
  Binary,
  Func
};
enum class SeriesName { Open, High, Low, Close, Volume };
constexpr int kSeriesCount = 5;
enum class StateName { Position, EntryPrice, Cash, Equity, BarsInPosition };
enum class BinOp { Or, And, Lt, Le, Gt, Ge, Eq, Ne, Add, Sub, Mul, Div };
enum class FuncOp { Abs, Sqrt, Min, Max };

using Idx = int;

struct CNode {
  NodeKind Kind = NodeKind::Const;
  double Value = 0; // Const
  SeriesName Series = SeriesName::Close;
  StateName State = StateName::Position;
  BinOp Bin = BinOp::Add;
  FuncOp Func = FuncOp::Abs;
  int Indicator = -1; // Indicator: номер в списке индикаторов
  int BarOffset = 0;  // Series и Indicator: смещение назад
  Idx A = -1, B = -1; // операнды
  bool IsBool =
      false; // логическое по типу: подстановка в because печатает «да» / «нет»
  bool NeedsState =
      false; // зависит от состояния портфеля (прямо или через индикатор)
};

// Индикатор с входами. Одинаковые вызовы (одна функция, одинаковые аргументы) -
// один экземпляр.
struct IndicatorSpec {
  std::string Name;
  std::vector<std::int64_t> IntArgs;
  std::vector<Idx> Inputs;
  bool NeedsState = false;
  int MaxOffset =
      0; // сколько баров истории результата нужно (смещения у вызова)
  bool IsBool = false;
};

struct CPart {
  std::string Text;
  Idx Expr = -1; // -1 - текстовая часть
};

struct CRule {
  std::string Name;
  Idx When = -1;
  Action::Kind ActionKind = Action::Kind::Cancel;
  SizeKind SizeUnit = SizeKind::Lots;
  Idx Size = -1;
  OrderKind OrderMode = OrderKind::Market;
  Idx Price = -1;
  TimeInForce Tif = TimeInForce::Gtc;
  std::vector<CPart> Because;
};

// Состояние одной бумаги.
struct InstrumentState {
  std::vector<Ring> Series; // open high low close volume
  std::vector<std::unique_ptr<Indicator>> Indicators;
  std::vector<Ring> History; // результаты индикаторов по барам
  std::optional<std::int64_t> BarStep; // шаг последнего onBar
  std::optional<std::int64_t> EvaluatedStep; // шаг последнего evaluate
};

} // namespace

// ---- Impl
// ----------------------------------------------------------------------------------

struct StrategyInstance::Impl {
  Program Prog; // своя копия: с переопределёнными параметрами, проверенная
                // заново
  std::map<std::string, double> ParamValues;
  std::vector<CNode> Nodes;
  std::vector<IndicatorSpec> Specs;
  std::map<std::string, int> SpecByKey; // упорядоченный контейнер: порядок не
                                        // зависит от адресов (INV-4)
  std::vector<CRule> Rules;
  int MaxSeriesOffset = 0;
  std::vector<InstrumentState> Instruments;

  // ---- компиляция ----

  Idx add(CNode Node) {
    Nodes.push_back(Node);
    return static_cast<Idx>(Nodes.size() - 1);
  }

  // Целое значение смещения или периода: литерал или параметр (проверено
  // check()).
  std::int64_t wholeValue(double V, const char *What) const {
    if (!(std::floor(V) == V) || V < 0 || V > static_cast<double>(kMaxBars)) {
      throw std::invalid_argument(std::string("недопустимое значение для ") +
                                  What);
    }
    return static_cast<std::int64_t>(V);
  }

  double paramValue(const std::string &Name) const {
    const auto Found = ParamValues.find(Name);
    if (Found == ParamValues.end())
      throw std::invalid_argument("неизвестное имя `" + Name + "`");
    return Found->second;
  }

  int resolveOffset(const Offset &Offset) const {
    if (Offset.Negative) {
      // Невозможно после check(): E-LA01. Защита от чтения будущего (INV-1).
      if (const auto *N = std::get_if<std::int64_t>(&Offset.Value);
          N != nullptr && *N == 0)
        return 0;
      throw std::invalid_argument("отрицательное смещение: чтение будущего");
    }
    if (const auto *N = std::get_if<std::int64_t>(&Offset.Value)) {
      return static_cast<int>(wholeValue(static_cast<double>(*N), "смещения"));
    }
    return static_cast<int>(wholeValue(
        paramValue(std::get<std::string>(Offset.Value)), "смещения"));
  }

  static bool seriesByName(const std::string &Name, SeriesName &Out) {
    if (Name == "open")
      Out = SeriesName::Open;
    else if (Name == "high")
      Out = SeriesName::High;
    else if (Name == "low")
      Out = SeriesName::Low;
    else if (Name == "close")
      Out = SeriesName::Close;
    else if (Name == "volume")
      Out = SeriesName::Volume;
    else
      return false;
    return true;
  }

  static bool stateByName(const std::string &Name, StateName &Out) {
    if (Name == "position")
      Out = StateName::Position;
    else if (Name == "entry_price")
      Out = StateName::EntryPrice;
    else if (Name == "cash")
      Out = StateName::Cash;
    else if (Name == "equity")
      Out = StateName::Equity;
    else if (Name == "bars_in_position")
      Out = StateName::BarsInPosition;
    else
      return false;
    return true;
  }

  Idx seriesNode(SeriesName Series, int Offset) {
    CNode Node;
    Node.Kind = NodeKind::Series;
    Node.Series = Series;
    Node.BarOffset = Offset;
    if (Offset > MaxSeriesOffset)
      MaxSeriesOffset = Offset;
    return add(Node);
  }

  Idx compile(const Expr &Expr) {
    struct Visitor {
      StrategyInstance::Impl &Self;
      Idx operator()(const Number &N) const {
        CNode Node;
        Node.Value = N.Value;
        return Self.add(Node);
      }
      Idx operator()(const Bool &B) const {
        CNode Node;
        Node.Value = B.Value ? 1.0 : 0.0;
        Node.IsBool = true;
        return Self.add(Node);
      }
      Idx operator()(const Na &) const {
        CNode Node;
        Node.Kind = NodeKind::Na;
        return Self.add(Node);
      }
      Idx operator()(const Ident &Ident) const {
        return Self.compileIdent(Ident);
      }
      Idx operator()(const Call &Call) const { return Self.compileCall(Call); }
      Idx operator()(const Unary &Unary) const {
        CNode Node;
        Node.A = Self.compile(*Unary.Operand);
        Node.Kind = Unary.Op == '!' ? NodeKind::Not : NodeKind::Neg;
        Node.IsBool = Unary.Op == '!';
        Node.NeedsState =
            Self.Nodes[static_cast<std::size_t>(Node.A)].NeedsState;
        return Self.add(Node);
      }
      Idx operator()(const Binary &Binary) const {
        return Self.compileBinary(Binary);
      }
    };
    return std::visit(Visitor{*this}, Expr.Node);
  }

  Idx compileIdent(const Ident &Ident) {
    SeriesName Series;
    StateName State;
    if (seriesByName(Ident.Name, Series)) {
      return seriesNode(Series,
                        Ident.BarOffset ? resolveOffset(*Ident.BarOffset) : 0);
    }
    if (stateByName(Ident.Name, State)) {
      if (Ident.BarOffset)
        throw std::invalid_argument(
            "смещение допускается только у рядов и индикаторов");
      CNode Node;
      Node.Kind = NodeKind::State;
      Node.State = State;
      Node.NeedsState = true;
      return add(Node);
    }
    if (Ident.BarOffset)
      throw std::invalid_argument(
          "смещение допускается только у рядов и индикаторов");
    CNode Node;
    Node.Value = paramValue(Ident.Name);
    return add(Node);
  }

  Idx compileBinary(const Binary &Binary) {
    static const std::pair<const char *, BinOp> kTable[] = {
        {"or", BinOp::Or}, {"and", BinOp::And}, {"<", BinOp::Lt},
        {"<=", BinOp::Le}, {">", BinOp::Gt},    {">=", BinOp::Ge},
        {"==", BinOp::Eq}, {"!=", BinOp::Ne},   {"+", BinOp::Add},
        {"-", BinOp::Sub}, {"*", BinOp::Mul},   {"/", BinOp::Div}};
    CNode Node;
    Node.Kind = NodeKind::Binary;
    bool Found = false;
    for (const auto &[text, op] : kTable) {
      if (Binary.Op == text) {
        Node.Bin = op;
        Found = true;
      }
    }
    if (!Found)
      throw std::invalid_argument("неизвестный оператор `" + Binary.Op + "`");
    Node.IsBool = Node.Bin <= BinOp::Ne;
    Node.A = compile(*Binary.Left);
    Node.B = compile(*Binary.Right);
    Node.NeedsState = Nodes[static_cast<std::size_t>(Node.A)].NeedsState ||
                      Nodes[static_cast<std::size_t>(Node.B)].NeedsState;
    return add(Node);
  }

  // Период индикатора: целый литерал или параметр.
  std::int64_t compilePeriod(const Expr &Arg) const {
    if (const auto *NumberNode = std::get_if<Number>(&Arg.Node))
      return wholeValue(NumberNode->Value, "периода");
    if (const auto *IdentNode = std::get_if<Ident>(&Arg.Node);
        IdentNode != nullptr && !IdentNode->BarOffset) {
      return wholeValue(paramValue(IdentNode->Name), "периода");
    }
    throw std::invalid_argument(
        "период должен быть целым числом или целочисленным параметром");
  }

  Idx compileCall(const Call &Call) {
    const std::string &Name = Call.Name;
    if (Name == "abs" || Name == "sqrt" || Name == "min" || Name == "max") {
      if (Call.BarOffset)
        throw std::invalid_argument(
            "смещение допускается только у рядов и индикаторов");
      CNode Node;
      Node.Kind = NodeKind::Func;
      Node.Func = Name == "abs"    ? FuncOp::Abs
                  : Name == "sqrt" ? FuncOp::Sqrt
                  : Name == "min"  ? FuncOp::Min
                                   : FuncOp::Max;
      const std::size_t Arity =
          (Node.Func == FuncOp::Min || Node.Func == FuncOp::Max) ? 2 : 1;
      if (Call.Args.size() != Arity)
        throw std::invalid_argument("неверное число аргументов `" + Name + "`");
      Node.A = compile(*Call.Args[0]);
      Node.NeedsState = Nodes[static_cast<std::size_t>(Node.A)].NeedsState;
      if (Arity == 2) {
        Node.B = compile(*Call.Args[1]);
        Node.NeedsState = Node.NeedsState ||
                          Nodes[static_cast<std::size_t>(Node.B)].NeedsState;
      }
      return add(Node);
    }

    // Индикаторы считает indicators.cpp; интерпретатор только подаёт им входы
    // на каждом баре, `na` - как есть. Для change(x, n) это значит: x - n-е
    // значение назад среди не-na значений x (окно из n + 1 не-na значений), а
    // не значение n баров назад; пока окно не заполнено - `na`.
    IndicatorSpec Spec;
    Spec.Name = Name;
    if (Name == "atr") {
      if (Call.Args.size() != 1)
        throw std::invalid_argument("неверное число аргументов `atr`");
      Spec.IntArgs = {compilePeriod(*Call.Args[0])};
      Spec.Inputs = {seriesNode(SeriesName::High, 0),
                     seriesNode(SeriesName::Low, 0),
                     seriesNode(SeriesName::Close, 0)};
    } else if (Name == "crossover" || Name == "crossunder") {
      if (Call.Args.size() != 2)
        throw std::invalid_argument("неверное число аргументов `" + Name + "`");
      Spec.Inputs = {compile(*Call.Args[0]), compile(*Call.Args[1])};
      Spec.IsBool = true;
    } else {
      // sma ema rsi stdev highest lowest change: (x, n)
      if (Call.Args.size() != 2)
        throw std::invalid_argument("неверное число аргументов `" + Name + "`");
      Spec.Inputs = {compile(*Call.Args[0])};
      Spec.IntArgs = {compilePeriod(*Call.Args[1])};
    }
    for (Idx Input : Spec.Inputs)
      Spec.NeedsState =
          Spec.NeedsState || Nodes[static_cast<std::size_t>(Input)].NeedsState;

    const std::string Key = specKey(Spec);
    int Index;
    if (const auto Found = SpecByKey.find(Key); Found != SpecByKey.end()) {
      Index = Found->second;
    } else {
      Index = static_cast<int>(Specs.size());
      Specs.push_back(Spec);
      SpecByKey.emplace(Key, Index);
    }
    const int Offset = Call.BarOffset ? resolveOffset(*Call.BarOffset) : 0;
    IndicatorSpec &Stored = Specs[static_cast<std::size_t>(Index)];
    if (Offset > Stored.MaxOffset)
      Stored.MaxOffset = Offset;

    CNode Node;
    Node.Kind = NodeKind::Indicator;
    Node.Indicator = Index;
    Node.BarOffset = Offset;
    Node.IsBool = Stored.IsBool;
    Node.NeedsState = Stored.NeedsState;
    return add(Node);
  }

  // Каноническая запись дерева: ключ дедупликации индикаторов. Параметры уже
  // подставлены значениями, поэтому `sma(close, fast)` при fast = 20 и
  // `sma(close, 20)` - один индикатор: состояние индикатора - чистая функция
  // потока входов.
  std::string keyOf(Idx Index) const {
    const CNode &Node = Nodes[static_cast<std::size_t>(Index)];
    switch (Node.Kind) {
    case NodeKind::Const: {
      char Buffer[64];
      const auto R = std::to_chars(Buffer, Buffer + sizeof(Buffer), Node.Value);
      return "c" + std::string(Buffer, R.ptr);
    }
    case NodeKind::Na:
      return "na";
    case NodeKind::Series:
      return "s" + std::to_string(static_cast<int>(Node.Series)) + "[" +
             std::to_string(Node.BarOffset) + "]";
    case NodeKind::State:
      return "p" + std::to_string(static_cast<int>(Node.State));
    case NodeKind::Indicator:
      return "i" + std::to_string(Node.Indicator) + "[" +
             std::to_string(Node.BarOffset) + "]";
    case NodeKind::Neg:
      return "(neg " + keyOf(Node.A) + ")";
    case NodeKind::Not:
      return "(not " + keyOf(Node.A) + ")";
    case NodeKind::Binary:
      return "(b" + std::to_string(static_cast<int>(Node.Bin)) + " " +
             keyOf(Node.A) + " " + keyOf(Node.B) + ")";
    case NodeKind::Func:
      return "(f" + std::to_string(static_cast<int>(Node.Func)) + " " +
             keyOf(Node.A) + (Node.B >= 0 ? " " + keyOf(Node.B) : "") + ")";
    }
    return "";
  }

  std::string specKey(const IndicatorSpec &Spec) const {
    std::string Key = Spec.Name;
    for (std::int64_t N : Spec.IntArgs)
      Key += ":" + std::to_string(N);
    for (Idx Input : Spec.Inputs)
      Key += "|" + keyOf(Input);
    return Key;
  }

  void compileRules() {
    for (const Rule &Rule : Prog.Rules) {
      CRule Compiled;
      Compiled.Name = Rule.Name;
      Compiled.When = compile(*Rule.When);
      Compiled.ActionKind = Rule.Then.ActionKind;
      Compiled.SizeUnit = Rule.Then.SizeUnit;
      if (Rule.Then.Size)
        Compiled.Size = compile(*Rule.Then.Size);
      Compiled.OrderMode = Rule.Then.OrderMode;
      if (Rule.Then.Price)
        Compiled.Price = compile(*Rule.Then.Price);
      Compiled.Tif =
          Rule.Then.Day.value_or(false) ? TimeInForce::Day : TimeInForce::Gtc;
      for (const auto &Part : Rule.Because.Parts) {
        CPart P;
        if (const auto *Text = std::get_if<std::string>(&Part)) {
          P.Text = *Text;
        } else {
          P.Expr = compile(*std::get<ExprPtr>(Part));
        }
        Compiled.Because.push_back(std::move(P));
      }
      Rules.push_back(std::move(Compiled));
    }
  }

  void makeInstruments(std::size_t Count) {
    Instruments.resize(Count);
    for (InstrumentState &Inst : Instruments) {
      Inst.Series.assign(kSeriesCount,
                         Ring(static_cast<std::size_t>(MaxSeriesOffset) + 1));
      for (const IndicatorSpec &Spec : Specs) {
        Inst.Indicators.push_back(makeIndicator(Spec.Name, Spec.IntArgs));
        Inst.History.emplace_back(static_cast<std::size_t>(Spec.MaxOffset) + 1);
      }
    }
  }

  // ---- вычисление ----

  struct Context {
    const InstrumentState &Inst;
    const StateView *State; // null при onBar: узлы без зависимости от состояния
                            // его не читают
  };

  Value stateValue(StateName Name, const StateView &S) const {
    switch (Name) {
    case StateName::Position:
      return static_cast<double>(S.PositionLots);
    case StateName::EntryPrice:
      return S.EntryPrice;
    case StateName::Cash:
      return S.Cash;
    case StateName::Equity:
      return S.Equity;
    case StateName::BarsInPosition:
      if (!S.BarsInPosition)
        return std::nullopt;
      return static_cast<double>(*S.BarsInPosition);
    }
    return std::nullopt;
  }

  Value eval(Idx Index, const Context &Ctx) const {
    const CNode &Node = Nodes[static_cast<std::size_t>(Index)];
    switch (Node.Kind) {
    case NodeKind::Const:
      return Node.Value;
    case NodeKind::Na:
      return std::nullopt;
    case NodeKind::Series:
      return Ctx.Inst.Series[static_cast<std::size_t>(Node.Series)].at(
          static_cast<std::size_t>(Node.BarOffset));
    case NodeKind::State:
      if (Ctx.State == nullptr)
        throw std::logic_error("состояние портфеля недоступно при onBar");
      {
        const Value V = stateValue(Node.State, *Ctx.State);
        return V ? finiteOrNa(*V)
                 : V; // состояние приходит от вызывающего: не конечное - na
      }
    case NodeKind::Indicator: { // выход индикатора мог выйти за double (сумма
                                // окна
      // переполнилась): это na, а не inf/NaN
      const Value V =
          Ctx.Inst.History[static_cast<std::size_t>(Node.Indicator)].at(
              static_cast<std::size_t>(Node.BarOffset));
      return V ? finiteOrNa(*V) : V;
    }
    case NodeKind::Neg: {
      const Value V = eval(Node.A, Ctx);
      if (!V)
        return std::nullopt;
      return -*V;
    }
    case NodeKind::Not:
      return fromBool(!truthy(eval(Node.A, Ctx))); // `na` в логике - false
    case NodeKind::Binary:
      return evalBinary(Node, Ctx);
    case NodeKind::Func:
      return evalFunc(Node, Ctx);
    }
    return std::nullopt;
  }

  Value evalBinary(const CNode &Node, const Context &Ctx) const {
    // and / or: правый операнд не вычисляется, если результат ясен. Индикаторы
    // это не затрагивает: они обновляются отдельно, до вычисления правил.
    if (Node.Bin == BinOp::Or)
      return fromBool(truthy(eval(Node.A, Ctx)) || truthy(eval(Node.B, Ctx)));
    if (Node.Bin == BinOp::And)
      return fromBool(truthy(eval(Node.A, Ctx)) && truthy(eval(Node.B, Ctx)));

    const Value Left = eval(Node.A, Ctx);
    const Value Right = eval(Node.B, Ctx);
    switch (Node.Bin) {
    case BinOp::Lt:
    case BinOp::Le:
    case BinOp::Gt:
    case BinOp::Ge:
    case BinOp::Eq:
    case BinOp::Ne: {
      if (!Left || !Right)
        return fromBool(false); // сравнение с na - false
      switch (Node.Bin) {
      case BinOp::Lt:
        return fromBool(*Left < *Right);
      case BinOp::Le:
        return fromBool(*Left <= *Right);
      case BinOp::Gt:
        return fromBool(*Left > *Right);
      case BinOp::Ge:
        return fromBool(*Left >= *Right);
      case BinOp::Eq:
        return fromBool(*Left == *Right);
      default:
        return fromBool(*Left != *Right);
      }
    }
    default:
      break;
    }
    if (!Left || !Right)
      return std::nullopt; // арифметика с na - na
    switch (Node.Bin) {
    case BinOp::Add:
      return finiteOrNa(*Left + *Right);
    case BinOp::Sub:
      return finiteOrNa(*Left - *Right);
    case BinOp::Mul:
      return finiteOrNa(*Left * *Right);
    default: // Div
      if (*Right == 0.0)
        return std::nullopt; // деление на 0 - na
      return finiteOrNa(*Left / *Right);
    }
  }

  Value evalFunc(const CNode &Node, const Context &Ctx) const {
    const Value A = eval(Node.A, Ctx);
    if (Node.Func == FuncOp::Abs)
      return A ? Value(std::fabs(*A)) : std::nullopt;
    if (Node.Func == FuncOp::Sqrt) {
      if (!A || *A < 0)
        return std::nullopt;
      return std::sqrt(*A);
    }
    const Value B = eval(Node.B, Ctx);
    if (!A || !B)
      return std::nullopt;
    if (Node.Func == FuncOp::Min)
      return *A < *B ? *A : *B;
    return *A > *B ? *A : *B;
  }

  // Обновить индикаторы бумаги, у которых зависимость от состояния равна
  // stateful. Порядок списка - порядок создания, входы создаются раньше самого
  // индикатора: обход топологический.
  void updateIndicators(InstrumentState &Inst, bool Stateful,
                        const StateView *State) const {
    const Context Ctx{Inst, State};
    std::vector<Value> Inputs;
    for (std::size_t I = 0; I < Specs.size(); ++I) {
      if (Specs[I].NeedsState != Stateful)
        continue;
      Inputs.clear();
      for (Idx Input : Specs[I].Inputs)
        Inputs.push_back(eval(Input, Ctx));
      Inst.History[I].push(Inst.Indicators[I]->update(Inputs));
    }
  }

  // ---- действия ----

  static std::optional<std::int64_t> toLots(double Floored) {
    if (!std::isfinite(Floored) || Floored < 1.0 || Floored >= 9.0e18)
      return std::nullopt;
    return static_cast<std::int64_t>(Floored);
  }

  std::optional<std::int64_t> computeLots(const CRule &Rule, const Context &Ctx,
                                          double Close,
                                          std::int64_t LotSize) const {
    const StateView &State = *Ctx.State;
    if (Rule.SizeUnit == SizeKind::All)
      return toLots(static_cast<double>(State.PositionLots));
    const Value N = eval(Rule.Size, Ctx);
    if (!N)
      return std::nullopt;
    switch (Rule.SizeUnit) {
    case SizeKind::Lots:
      return toLots(std::floor(*N));
    case SizeKind::PercentEquity:
    case SizeKind::PercentCash: {
      const double Base =
          Rule.SizeUnit == SizeKind::PercentEquity ? State.Equity : State.Cash;
      const double LotCost = Close * static_cast<double>(LotSize);
      if (!(LotCost > 0))
        return std::nullopt;
      return toLots(std::floor(Base * *N / 100 / LotCost));
    }
    case SizeKind::PercentPosition:
      return toLots(
          std::floor(static_cast<double>(State.PositionLots) * *N / 100));
    case SizeKind::All:
      break;
    }
    return std::nullopt;
  }

  std::string renderBecause(const CRule &Rule, const Context &Ctx) const {
    std::string Out;
    for (const CPart &Part : Rule.Because) {
      if (Part.Expr < 0) {
        Out += Part.Text;
        continue;
      }
      const Value V = eval(Part.Expr, Ctx);
      if (!V) {
        Out += "н/д";
      } else if (Nodes[static_cast<std::size_t>(Part.Expr)].IsBool) {
        Out += *V != 0.0 ? "да" : "нет";
      } else {
        Out += formatNumber(*V);
      }
    }
    return Out;
  }

  std::optional<Intent> makeIntent(const CRule &Rule, int Instrument,
                                   const Context &Ctx, double Close,
                                   std::int64_t LotSize,
                                   const Decimal &PriceStep) const {
    Intent Intent;
    Intent.Instrument = Instrument;
    Intent.RuleName = Rule.Name;
    Intent.Order.Instrument = Instrument;
    if (Rule.ActionKind == Action::Kind::Cancel) {
      Intent.CancelAll = true;
      Intent.Because = renderBecause(Rule, Ctx);
      return Intent;
    }

    const std::optional<std::int64_t> Lots =
        computeLots(Rule, Ctx, Close, LotSize);
    if (!Lots)
      return std::nullopt;
    Intent.Order.OrderSide =
        Rule.ActionKind == Action::Kind::Buy ? Side::Buy : Side::Sell;
    Intent.Order.Lots = *Lots;
    Intent.Order.Tif = Rule.Tif;

    if (Rule.OrderMode == OrderKind::Market) {
      Intent.Order.Type = OrderType::Market;
    } else {
      const Value P = eval(Rule.Price, Ctx);
      if (!P || !std::isfinite(*P) || !(*P > 0))
        return std::nullopt;
      Decimal Price;
      try {
        Price = toDecimalPrice(*P, PriceStep);
      } catch (const DecimalOverflow &) {
        return std::nullopt; // цена за пределами денежного типа - заявку не
                             // создаём
      }
      if (Price.sign() <= 0)
        return std::nullopt; // округлилась до 0
      switch (Rule.OrderMode) {
      case OrderKind::Limit:
        Intent.Order.Type = OrderType::Limit;
        Intent.Order.LimitPrice = Price;
        break;
      case OrderKind::Stop:
        Intent.Order.Type = OrderType::Stop;
        Intent.Order.StopPrice = Price;
        break;
      default: // TakeProfit: цена - в limitPrice (exec/order.hpp)
        Intent.Order.Type = OrderType::TakeProfit;
        Intent.Order.LimitPrice = Price;
        break;
      }
    }
    Intent.Because = renderBecause(Rule, Ctx);
    return Intent;
  }
};

// ---- публичный интерфейс
// -----------------------------------------------------------------------

Decimal toDecimalPrice(double Price, const Decimal &PriceStep) {
  if (!std::isfinite(Price))
    throw std::invalid_argument("цена не является конечным числом");
  return Decimal::parse(fixed9(Price))
      .roundToStep(PriceStep, RoundingMode::HalfUp);
}

StrategyInstance::StrategyInstance(const Program &Program,
                                   const std::vector<ResolvedParam> &Params,
                                   const ParamValues &Overrides,
                                   std::size_t InstrumentCount)
    : Pimpl(std::make_unique<Impl>()) {
  if (InstrumentCount < 1)
    throw std::invalid_argument("в сессии нет бумаг");
  Pimpl->Prog = cloneProgram(Program);

  for (const auto &[name, value] : Overrides) {
    const ResolvedParam *Declared = nullptr;
    for (const ResolvedParam &P : Params) {
      if (P.Name == name)
        Declared = &P;
    }
    if (Declared == nullptr)
      throw std::invalid_argument("неизвестный параметр `" + name + "`");
    if (!std::isfinite(value))
      throw std::invalid_argument("параметр `" + name + "`: значение не число");
    if ((Declared->Min && value < *Declared->Min) ||
        (Declared->Max && value > *Declared->Max)) {
      throw std::invalid_argument("параметр `" + name +
                                  "`: значение вне диапазона");
    }
    for (ParamDecl &Decl : Pimpl->Prog.Params) {
      if (Decl.Name == name) {
        Decl.Value = value;
        break; // первое объявление: как в check()
      }
    }
  }

  // Программа с новыми значениями проверяется заново (INV-1): `k = -1` в
  // `close[k]`, `n = 0` или `n = 100000` в `sma(close, n)` дают ошибку, и
  // экземпляр не создаётся. Проверка идёт и без переопределений: интерпретатор
  // не исполняет программу с ошибками.
  const CheckResult Checked = check(Pimpl->Prog);
  if (Checked.Severity == Level::Error) {
    for (const Diagnostic &D : Checked.Diagnostics) {
      if (D.Severity == Level::Error) {
        throw std::invalid_argument("программа не проходит проверку (" +
                                    D.Code + "): " + D.Message);
      }
    }
  }
  for (const ResolvedParam &P : Checked.Params)
    Pimpl->ParamValues.emplace(P.Name, P.Value);

  Pimpl->compileRules();
  Pimpl->makeInstruments(InstrumentCount);
}

StrategyInstance::~StrategyInstance() = default;
StrategyInstance::StrategyInstance(StrategyInstance &&) noexcept = default;
StrategyInstance &
StrategyInstance::operator=(StrategyInstance &&) noexcept = default;

void StrategyInstance::onBar(int Instrument, const StepBar &Bar) {
  if (!Pimpl)
    throw std::logic_error("onBar: экземпляр перемещён");
  if (Instrument < 0 ||
      static_cast<std::size_t>(Instrument) >= Pimpl->Instruments.size()) {
    throw std::out_of_range("onBar: бумаги с таким номером нет в сессии");
  }
  if (Bar.Instrument != Instrument)
    throw std::invalid_argument("onBar: бар другой бумаги");
  InstrumentState &Inst =
      Pimpl->Instruments[static_cast<std::size_t>(Instrument)];
  if (Inst.BarStep && Bar.Step <= *Inst.BarStep) {
    throw std::invalid_argument("onBar: шаг бара должен возрастать");
  }
  if (Inst.BarStep && Inst.EvaluatedStep != Inst.BarStep) {
    // Бар без evaluate: индикаторы от состояния портфеля потеряли бы отсчёт.
    // Контракт с прогоном (задача 21).
    throw std::logic_error("onBar: предыдущий бар не вычислен через evaluate");
  }

  Inst.Series[static_cast<std::size_t>(SeriesName::Open)].push(
      Bar.Open.toDouble());
  Inst.Series[static_cast<std::size_t>(SeriesName::High)].push(
      Bar.High.toDouble());
  Inst.Series[static_cast<std::size_t>(SeriesName::Low)].push(
      Bar.Low.toDouble());
  Inst.Series[static_cast<std::size_t>(SeriesName::Close)].push(
      Bar.Close.toDouble());
  Inst.Series[static_cast<std::size_t>(SeriesName::Volume)].push(
      static_cast<double>(Bar.VolumeLots));
  Inst.BarStep = Bar.Step;
  Pimpl->updateIndicators(Inst, false, nullptr);
}

std::vector<Intent> StrategyInstance::evaluate(int Instrument,
                                               const StepBar &Bar,
                                               std::int64_t LotSize,
                                               const Decimal &PriceStep,
                                               const StateView &State) {
  if (!Pimpl)
    throw std::logic_error("evaluate: экземпляр перемещён");
  if (Instrument < 0 ||
      static_cast<std::size_t>(Instrument) >= Pimpl->Instruments.size()) {
    throw std::out_of_range("evaluate: бумаги с таким номером нет в сессии");
  }
  if (LotSize < 1)
    throw std::invalid_argument(
        "evaluate: размер лота должен быть не меньше 1");
  if (PriceStep.sign() <= 0)
    throw std::invalid_argument("evaluate: шаг цены должен быть положительным");
  InstrumentState &Inst =
      Pimpl->Instruments[static_cast<std::size_t>(Instrument)];
  if (Bar.Instrument != Instrument || !Inst.BarStep ||
      *Inst.BarStep != Bar.Step) {
    throw std::logic_error(
        "evaluate: бар не совпадает с последним, переданным в onBar");
  }
  if (Inst.EvaluatedStep && *Inst.EvaluatedStep == Bar.Step) {
    throw std::logic_error("evaluate: бар уже вычислен");
  }
  Inst.EvaluatedStep = Bar.Step;

  Pimpl->updateIndicators(Inst, true, &State);

  const StrategyInstance::Impl::Context Ctx{Inst, &State};
  const double Close = Bar.Close.toDouble();
  std::vector<Intent> Intents;
  for (const CRule &Rule : Pimpl->Rules) {
    if (!truthy(Pimpl->eval(Rule.When, Ctx)))
      continue;
    if (auto Intent = Pimpl->makeIntent(Rule, Instrument, Ctx, Close, LotSize,
                                        PriceStep)) {
      Intents.push_back(std::move(*Intent));
    }
  }
  return Intents;
}

} // namespace core::dsl
