#include "convert/diagnostics.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

namespace worker::convert {

pb::Level toProto(core::dsl::Level Level) {
  switch (Level) {
  case core::dsl::Level::Ok:
    return pb::OK;
  case core::dsl::Level::Warning:
    return pb::WARNING;
  case core::dsl::Level::Error:
    return pb::ERROR;
  }
  return pb::LEVEL_UNSPECIFIED;
}

pb::Diagnostic toProto(const core::dsl::Diagnostic &Diagnostic) {
  pb::Diagnostic Out;
  Out.set_code(Diagnostic.Code);
  Out.set_level(toProto(Diagnostic.Severity));
  Out.set_start_line(Diagnostic.Range.StartLine);
  Out.set_start_column(Diagnostic.Range.StartColumn);
  Out.set_end_line(Diagnostic.Range.EndLine);
  Out.set_end_column(Diagnostic.Range.EndColumn);
  Out.set_message(Diagnostic.Message);
  for (const core::dsl::TextEdit &Edit : Diagnostic.Fix) {
    pb::TextEdit *E = Out.add_fix();
    E->set_start_line(Edit.Range.StartLine);
    E->set_start_column(Edit.Range.StartColumn);
    E->set_end_line(Edit.Range.EndLine);
    E->set_end_column(Edit.Range.EndColumn);
    E->set_text(Edit.Text);
  }
  return Out;
}

pb::ParamDecl toProto(const core::dsl::ResolvedParam &Param) {
  pb::ParamDecl Out;
  Out.set_name(Param.Name);
  Out.set_default_value(Param.Value);
  Out.set_integer(Param.Integer);
  // Диапазон и шаг в DSL задаются вместе; без диапазона поля остаются нулевыми.
  if (Param.Min && Param.Max && Param.Step) {
    Out.set_has_range(true);
    Out.set_min(*Param.Min);
    Out.set_max(*Param.Max);
    Out.set_step(*Param.Step);
  }
  return Out;
}

std::vector<core::dsl::Diagnostic>
sortedByPosition(std::vector<core::dsl::Diagnostic> Diagnostics) {
  std::stable_sort(
      Diagnostics.begin(), Diagnostics.end(),
      [](const core::dsl::Diagnostic &A, const core::dsl::Diagnostic &B) {
        return std::tie(A.Range.StartLine, A.Range.StartColumn, A.Code) <
               std::tie(B.Range.StartLine, B.Range.StartColumn, B.Code);
      });
  return Diagnostics;
}

} // namespace worker::convert
