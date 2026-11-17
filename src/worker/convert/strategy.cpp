#include "convert/strategy.hpp"

#include <stdexcept>
#include <utility>

#include "core/dsl/parser.hpp"

namespace worker {

std::string errorCodes(const std::vector<core::dsl::Diagnostic> &diagnostics) {
  std::string codes;
  for (const core::dsl::Diagnostic &d : diagnostics) {
    if (d.level != core::dsl::Level::Error)
      continue;
    if (!codes.empty())
      codes += ';';
    codes += d.code;
  }
  return codes;
}

Strategy loadStrategy(const std::string &source) {
  core::dsl::ParseResult parsed = core::dsl::parse(source);
  if (!parsed.program)
    throw std::invalid_argument("стратегия не разобрана: " +
                                errorCodes(parsed.diagnostics));
  core::dsl::CheckResult checked = core::dsl::check(*parsed.program);
  if (checked.level == core::dsl::Level::Error) {
    throw std::invalid_argument("стратегия не проходит проверку: " +
                                errorCodes(checked.diagnostics));
  }
  return Strategy{std::move(*parsed.program), std::move(checked.params),
                  checked.level};
}

} // namespace worker
