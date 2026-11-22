#include <map>
#include <string>
#include <utility>

#include "convert/market.hpp"
#include "convert/run.hpp"
#include "dsl/check.hpp"
#include "handlers.hpp"

namespace worker {

namespace {

void fill(const pb::CheckStrategyRequest &Request,
          pb::CheckStrategyResponse *Out) {
  const std::map<std::string, double> Overrides =
      parseParamOverrides(Request.param_overrides());

  const dsl::CheckResult Checked = dsl::check(Request.source(), Overrides);
  Out->set_lookahead_level(toProto(Checked.Lookahead));

  for (const dsl::Diagnostic &Diag : Checked.Diagnostics) {
    toProto(Diag, Out->add_diagnostics());
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
