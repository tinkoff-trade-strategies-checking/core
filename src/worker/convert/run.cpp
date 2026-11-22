#include "convert/run.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

#include "convert/market.hpp"
#include "convert/report.hpp"
#include "convert/state.hpp"
#include "core/engine/digest.hpp"

namespace worker {

std::map<std::string, double> parseParamOverrides(
    const google::protobuf::RepeatedPtrField<pb::ParamValue> &Overrides) {
  std::map<std::string, double> Result;
  for (const pb::ParamValue &Param : Overrides) {
    if (!Result.emplace(Param.name(), Param.value()).second) {
      throw std::invalid_argument("дублирующийся параметр в param_overrides: " +
                                  Param.name());
    }
  }
  return Result;
}

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

void toProto(const dsl::Diagnostic &Diag, pb::Diagnostic *Out) {
  Out->set_code(Diag.Code);
  Out->set_level(toProto(Diag.Severity));
  Out->set_start_line(static_cast<std::int32_t>(Diag.Range.Start.Line));
  Out->set_start_column(static_cast<std::int32_t>(Diag.Range.Start.Column));
  Out->set_end_line(static_cast<std::int32_t>(Diag.Range.End.Line));
  Out->set_end_column(static_cast<std::int32_t>(Diag.Range.End.Column));
  Out->set_message(Diag.Message);
  for (const dsl::DiagnosticFix &Fix : Diag.Fixes) {
    pb::DiagnosticFix *DstFix = Out->add_fix();
    DstFix->set_description(Fix.Description);
    for (const dsl::TextEdit &Edit : Fix.Edits) {
      pb::TextEdit *DstEdit = DstFix->add_edits();
      DstEdit->set_start_line(static_cast<std::int32_t>(Edit.Range.Start.Line));
      DstEdit->set_start_column(
          static_cast<std::int32_t>(Edit.Range.Start.Column));
      DstEdit->set_end_line(static_cast<std::int32_t>(Edit.Range.End.Line));
      DstEdit->set_end_column(static_cast<std::int32_t>(Edit.Range.End.Column));
      DstEdit->set_replacement(Edit.Replacement);
    }
  }
}

void toProto(const core::StrategyRunResult &Run, const core::Report *ReportOpt,
             const core::Timeline &Timeline, bool IncludeHistory,
             pb::RunResult *Out) {
  Out->set_total_steps(Timeline.totalSteps());
  for (const dsl::Diagnostic &Diag : Run.Diagnostics) {
    toProto(Diag, Out->add_diagnostics());
  }

  if (!Run.Compiled || Run.Invalid) {
    Out->set_status(pb::INVALID);
    Out->set_level(pb::ERROR);
  } else if (Run.Engine.has_value() &&
             Run.Engine->status() == core::SessionStatus::Bankrupt) {
    Out->set_status(pb::BANKRUPT_STOP);
    Out->set_level(ReportOpt != nullptr ? toProto(ReportOpt->ReportLevel)
                                        : toProto(Run.FinalLevel));
  } else {
    Out->set_status(pb::COMPLETED);
    Out->set_level(ReportOpt != nullptr ? toProto(ReportOpt->ReportLevel)
                                        : toProto(Run.FinalLevel));
  }

  if (ReportOpt != nullptr) {
    toProto(*ReportOpt, Timeline, Out->mutable_report());
  }

  if (Run.Engine.has_value()) {
    const core::SessionEngine &Engine = *Run.Engine;
    const core::Decimal InitialCash = Engine.equity().front().Equity;
    for (const core::Fill &F : Engine.fills()) {
      toProto(F, Timeline, InitialCash, Out->add_fills());
    }
    for (const core::Order &O : Engine.orders()) {
      toProto(O, Timeline, Out->add_orders());
    }
    for (const core::JournalEntry &J : Run.Journal) {
      toProto(J, Timeline, Out->add_journal());
    }
    Out->set_digest(core::digestOf(Engine));
    const std::int64_t LastStep = Engine.currentStep();
    Out->set_last_step(LastStep);
    if (IncludeHistory && LastStep >= 0) {
      appendBars(Timeline, -Timeline.warmupSteps(), LastStep,
                 Out->mutable_history());
    }
  } else {
    Out->set_last_step(-1);
    Out->set_digest(0);
  }
}

} // namespace worker
