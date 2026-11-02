#include "convert/diagnostics.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

namespace worker::convert {

pb::Level toProto(core::dsl::Level level) {
  switch (level) {
    case core::dsl::Level::Ok:
      return pb::OK;
    case core::dsl::Level::Warning:
      return pb::WARNING;
    case core::dsl::Level::Error:
      return pb::ERROR;
  }
  return pb::LEVEL_UNSPECIFIED;
}

pb::Diagnostic toProto(const core::dsl::Diagnostic& diagnostic) {
  pb::Diagnostic out;
  out.set_code(diagnostic.code);
  out.set_level(toProto(diagnostic.level));
  out.set_start_line(diagnostic.range.startLine);
  out.set_start_column(diagnostic.range.startColumn);
  out.set_end_line(diagnostic.range.endLine);
  out.set_end_column(diagnostic.range.endColumn);
  out.set_message(diagnostic.message);
  for (const core::dsl::TextEdit& edit : diagnostic.fix) {
    pb::TextEdit* e = out.add_fix();
    e->set_start_line(edit.range.startLine);
    e->set_start_column(edit.range.startColumn);
    e->set_end_line(edit.range.endLine);
    e->set_end_column(edit.range.endColumn);
    e->set_text(edit.text);
  }
  return out;
}

pb::ParamDecl toProto(const core::dsl::ResolvedParam& param) {
  pb::ParamDecl out;
  out.set_name(param.name);
  out.set_default_value(param.value);
  out.set_integer(param.integer);
  // Диапазон и шаг в DSL задаются вместе; без диапазона поля остаются нулевыми.
  if (param.min && param.max && param.step) {
    out.set_has_range(true);
    out.set_min(*param.min);
    out.set_max(*param.max);
    out.set_step(*param.step);
  }
  return out;
}

std::vector<core::dsl::Diagnostic> sortedByPosition(std::vector<core::dsl::Diagnostic> diagnostics) {
  std::stable_sort(diagnostics.begin(), diagnostics.end(),
                   [](const core::dsl::Diagnostic& a, const core::dsl::Diagnostic& b) {
                     return std::tie(a.range.startLine, a.range.startColumn, a.code) <
                            std::tie(b.range.startLine, b.range.startColumn, b.code);
                   });
  return diagnostics;
}

}  // namespace worker::convert
