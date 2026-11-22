#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

#include "convert/market.hpp"
#include "dsl/check.hpp"
#include "handlers.hpp"

namespace worker {

namespace {

pb::Level toProto(dsl::LookaheadLevel Lookahead) {
  switch (Lookahead) {
  case dsl::LookaheadLevel::Ok:
    return pb::OK;
  case dsl::LookaheadLevel::Warning:
    return pb::WARNING;
  case dsl::LookaheadLevel::Error:
    return pb::ERROR;
  }
  return pb::LEVEL_UNSPECIFIED;
}

pb::Level toProto(dsl::DiagnosticSeverity Severity) {
  switch (Severity) {
  case dsl::DiagnosticSeverity::Error:
    return pb::ERROR;
  case dsl::DiagnosticSeverity::Warning:
    return pb::WARNING;
  }
  return pb::LEVEL_UNSPECIFIED;
}

void fill(const pb::CheckStrategyRequest &Request,
          pb::CheckStrategyResponse *Out) {
  std::map<std::string, double> Overrides;
  for (const pb::ParamValue &Param : Request.param_overrides()) {
    if (!Overrides.emplace(Param.name(), Param.value()).second) {
      throw std::invalid_argument("дублирующийся параметр в param_overrides: " +
                                  Param.name());
    }
  }

  const dsl::CheckResult Checked = dsl::check(Request.source(), Overrides);
  Out->set_lookahead_level(toProto(Checked.Lookahead));

  for (const dsl::Diagnostic &Diag : Checked.Diagnostics) {
    pb::Diagnostic *Dst = Out->add_diagnostics();
    Dst->set_code(Diag.Code);
    Dst->set_level(toProto(Diag.Severity));
    Dst->set_start_line(static_cast<std::int32_t>(Diag.Range.Start.Line));
    Dst->set_start_column(static_cast<std::int32_t>(Diag.Range.Start.Column));
    Dst->set_end_line(static_cast<std::int32_t>(Diag.Range.End.Line));
    Dst->set_end_column(static_cast<std::int32_t>(Diag.Range.End.Column));
    Dst->set_message(Diag.Message);
    for (const dsl::DiagnosticFix &Fix : Diag.Fixes) {
      pb::DiagnosticFix *DstFix = Dst->add_fix();
      DstFix->set_description(Fix.Description);
      for (const dsl::TextEdit &Edit : Fix.Edits) {
        pb::TextEdit *DstEdit = DstFix->add_edits();
        DstEdit->set_start_line(
            static_cast<std::int32_t>(Edit.Range.Start.Line));
        DstEdit->set_start_column(
            static_cast<std::int32_t>(Edit.Range.Start.Column));
        DstEdit->set_end_line(static_cast<std::int32_t>(Edit.Range.End.Line));
        DstEdit->set_end_column(
            static_cast<std::int32_t>(Edit.Range.End.Column));
        DstEdit->set_replacement(Edit.Replacement);
      }
    }
  }

  for (const dsl::ParamDecl &Param : Checked.Params) {
    pb::ParamDecl *Dst = Out->add_params();
    Dst->set_name(Param.Name);
    Dst->set_default_value(Param.DefaultValue);
    if (Param.RangeMin.has_value())
      Dst->set_range_min(*Param.RangeMin);
    if (Param.RangeMax.has_value())
      Dst->set_range_max(*Param.RangeMax);
    if (Param.StepText.has_value())
      Dst->set_step(*Param.StepText);
    Dst->set_integer(Param.IsInteger);
  }
}

} // namespace

// Ни одно исключение не выходит наружу (statusFromCurrentException).
grpc::Status handleCheckStrategy(const pb::CheckStrategyRequest &Request,
                                 pb::CheckStrategyResponse *Response) {
  try {
    pb::CheckStrategyResponse Result;
    fill(Request, &Result);
    *Response = std::move(Result);
    return grpc::Status::OK;
  } catch (...) {
    return statusFromCurrentException();
  }
}

} // namespace worker
