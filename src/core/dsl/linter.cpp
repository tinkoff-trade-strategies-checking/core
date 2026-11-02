#include "core/dsl/linter.hpp"

#include <set>
#include <string>
#include <utility>
#include <variant>

namespace core::dsl {
namespace {

// Куда попал литерал: от этого зависит подсказка для имени.
struct Context {
  enum class Kind { None, FunctionArgument, ComparedWithCall } kind = Kind::None;
  std::string function;
};

bool isComparison(const std::string& op) {
  return op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=";
}

bool isAllowed(double value) { return value == 0 || value == 1 || value == 100; }

class Linter {
 public:
  explicit Linter(const Program& program) : program_(program) {
    for (const ParamDecl& param : program.params) used_.insert(param.name);
  }

  std::vector<Diagnostic> run() {
    for (const Rule& rule : program_.rules) {
      visit(*rule.when, {});
      if (rule.action.size) visit(*rule.action.size, {});
      if (rule.action.price) visit(*rule.action.price, {});
      for (const auto& part : rule.because.parts) {
        if (const auto* expr = std::get_if<ExprPtr>(&part)) visit(**expr, {});
      }
    }
    return std::move(diagnostics_);
  }

 private:
  void visit(const Expr& expr, const Context& context) {
    struct Visitor {
      Linter& self;
      const Expr& expr;
      const Context& context;
      void operator()(const Number& number) const { self.report(expr, number, context); }
      void operator()(const Bool&) const {}
      void operator()(const Na&) const {}
      void operator()(const Ident&) const {}
      void operator()(const Call& call) const {
        for (const ExprPtr& arg : call.args) self.visit(*arg, {Context::Kind::FunctionArgument, call.name});
      }
      void operator()(const Unary& unary) const { self.visit(*unary.operand, context); }
      void operator()(const Binary& binary) const {
        if (!isComparison(binary.op)) {
          self.visit(*binary.left, {});
          self.visit(*binary.right, {});
          return;
        }
        self.visit(*binary.left, comparedWith(*binary.right));
        self.visit(*binary.right, comparedWith(*binary.left));
      }
    };
    std::visit(Visitor{*this, expr, context}, expr.node);
  }

  // Подсказка для литерала, сравниваемого с `other`: имя вызова либо никакой.
  static Context comparedWith(const Expr& other) {
    if (const auto* call = std::get_if<Call>(&other.node)) return {Context::Kind::ComparedWithCall, call->name};
    return {};
  }

  void report(const Expr& expr, const Number& number, const Context& context) {
    if (isAllowed(number.value)) return;
    const std::string name = uniqueName(baseName(context));

    Diagnostic diagnostic;
    diagnostic.code = "L001";
    diagnostic.level = Level::Warning;
    diagnostic.range = expr.range;
    diagnostic.message = "Магическая константа " + number.text + ": вынесите её в параметр " + name;
    // Вставка первой: обе правки заданы в координатах исходного текста.
    const int line = program_.rules.front().range.startLine;
    diagnostic.fix.push_back({{line, 1, line, 1}, "param " + name + " = " + number.text + "\n"});
    diagnostic.fix.push_back({expr.range, name});
    diagnostics_.push_back(std::move(diagnostic));
  }

  // Имя до проверки на занятость. Резервный счётчик растёт на каждый такой литерал.
  std::string baseName(const Context& context) {
    switch (context.kind) {
      case Context::Kind::FunctionArgument:
        return context.function + "_n";
      case Context::Kind::ComparedWithCall:
        return context.function + "_level";
      case Context::Kind::None:
        break;
    }
    return "p" + std::to_string(++fallbackCount_);
  }

  std::string uniqueName(const std::string& base) {
    std::string name = base;
    for (int suffix = 2; used_.contains(name); ++suffix) name = base + "_" + std::to_string(suffix);
    used_.insert(name);
    return name;
  }

  const Program& program_;
  std::set<std::string> used_;  // имена параметров и уже предложенные имена; set - детерминизм
  int fallbackCount_ = 0;
  std::vector<Diagnostic> diagnostics_;
};

}  // namespace

std::vector<Diagnostic> lint(const Program& program, std::string_view /*source*/) {
  if (program.rules.empty()) return {};
  return Linter(program).run();
}

}  // namespace core::dsl
