#include "convert/strategy.hpp"

#include <stdexcept>
#include <utility>

#include "core/dsl/parser.hpp"

namespace worker {

std::string errorCodes(const std::vector<core::dsl::Diagnostic> &Diagnostics) {
  std::string Codes;
  for (const core::dsl::Diagnostic &D : Diagnostics) {
    if (D.Severity != core::dsl::Level::Error)
      continue;
    if (!Codes.empty())
      Codes += ';';
    Codes += D.Code;
  }
  return Codes;
}

Strategy loadStrategy(const std::string &Source) {
  core::dsl::ParseResult Parsed = core::dsl::parse(Source);
  if (!Parsed.Ast)
    throw std::invalid_argument("стратегия не разобрана: " +
                                errorCodes(Parsed.Diagnostics));
  core::dsl::CheckResult Checked = core::dsl::check(*Parsed.Ast);
  if (Checked.Severity == core::dsl::Level::Error) {
    throw std::invalid_argument("стратегия не проходит проверку: " +
                                errorCodes(Checked.Diagnostics));
  }
  return Strategy{std::move(*Parsed.Ast), std::move(Checked.Params),
                  Checked.Severity};
}

} // namespace worker
