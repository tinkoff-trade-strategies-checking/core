#include "core/dsl/linter.hpp"

#include <set>
#include <string>
#include <utility>
#include <variant>

namespace core::dsl {
namespace {

// Куда попал литерал: от этого зависит подсказка для имени.
struct Context {
  enum class Kind {
    None,
    FunctionArgument,
    ComparedWithCall
  } Kind = Kind::None;
  std::string Function;
};

bool isComparison(const std::string &Op) {
  return Op == "<" || Op == "<=" || Op == ">" || Op == ">=" || Op == "==" ||
         Op == "!=";
}

bool isAllowed(double Value) {
  return Value == 0 || Value == 1 || Value == 100;
}

class Linter {
public:
  explicit Linter(const Program &Program) : Prog(Program) {
    for (const ParamDecl &Param : Program.Params)
      Used.insert(Param.Name);
  }

  std::vector<Diagnostic> run() {
    for (const Rule &Rule : Prog.Rules) {
      visit(*Rule.When, {});
      if (Rule.Then.Size)
        visit(*Rule.Then.Size, {});
      if (Rule.Then.Price)
        visit(*Rule.Then.Price, {});
      for (const auto &Part : Rule.Because.Parts) {
        if (const auto *Expr = std::get_if<ExprPtr>(&Part))
          visit(**Expr, {});
      }
    }
    return std::move(Diagnostics);
  }

private:
  void visit(const Expr &E, const Context &Ctx) {
    struct Visitor {
      Linter &Self;
      const Expr &E;
      const Context &Ctx;
      void operator()(const Number &Number) const {
        Self.report(E, Number, Ctx);
      }
      void operator()(const Bool &) const {}
      void operator()(const Na &) const {}
      void operator()(const Ident &) const {}
      void operator()(const Call &Call) const {
        for (const ExprPtr &Arg : Call.Args)
          Self.visit(*Arg, {Context::Kind::FunctionArgument, Call.Name});
      }
      void operator()(const Unary &Unary) const {
        Self.visit(*Unary.Operand, Ctx);
      }
      void operator()(const Binary &Binary) const {
        if (!isComparison(Binary.Op)) {
          Self.visit(*Binary.Left, {});
          Self.visit(*Binary.Right, {});
          return;
        }
        Self.visit(*Binary.Left, comparedWith(*Binary.Right));
        Self.visit(*Binary.Right, comparedWith(*Binary.Left));
      }
    };
    std::visit(Visitor{*this, E, Ctx}, E.Node);
  }

  // Подсказка для литерала, сравниваемого с `other`: имя вызова либо никакой.
  static Context comparedWith(const Expr &Other) {
    if (const auto *CallNode = std::get_if<Call>(&Other.Node))
      return {Context::Kind::ComparedWithCall, CallNode->Name};
    return {};
  }

  void report(const Expr &Expr, const Number &Number, const Context &Context) {
    if (isAllowed(Number.Value))
      return;
    const std::string Name = uniqueName(baseName(Context));

    Diagnostic Diagnostic;
    Diagnostic.Code = "L001";
    Diagnostic.Severity = Level::Warning;
    Diagnostic.Range = Expr.Range;
    Diagnostic.Message = "Магическая константа " + Number.Text +
                         ": вынесите её в параметр " + Name;
    // Вставка первой: обе правки заданы в координатах исходного текста.
    const int Line = Prog.Rules.front().Range.StartLine;
    Diagnostic.Fix.push_back(
        {{Line, 1, Line, 1}, "param " + Name + " = " + Number.Text + "\n"});
    Diagnostic.Fix.push_back({Expr.Range, Name});
    Diagnostics.push_back(std::move(Diagnostic));
  }

  // Имя до проверки на занятость. Резервный счётчик растёт на каждый такой
  // литерал.
  std::string baseName(const Context &Context) {
    switch (Context.Kind) {
    case Context::Kind::FunctionArgument:
      return Context.Function + "_n";
    case Context::Kind::ComparedWithCall:
      return Context.Function + "_level";
    case Context::Kind::None:
      break;
    }
    return "p" + std::to_string(++FallbackCount);
  }

  std::string uniqueName(const std::string &Base) {
    std::string Name = Base;
    for (int Suffix = 2; Used.contains(Name); ++Suffix)
      Name = Base + "_" + std::to_string(Suffix);
    Used.insert(Name);
    return Name;
  }

  const Program &Prog;
  std::set<std::string>
      Used; // имена параметров и уже предложенные имена; set - детерминизм
  int FallbackCount = 0;
  std::vector<Diagnostic> Diagnostics;
};

} // namespace

std::vector<Diagnostic> lint(const Program &Program,
                             std::string_view /*source*/) {
  if (Program.Rules.empty())
    return {};
  return Linter(Program).run();
}

} // namespace core::dsl
