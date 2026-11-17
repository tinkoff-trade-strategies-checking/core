#include <stdexcept>
#include <utility>
#include <vector>

#include "clickhouse/clickhouse_client.hpp"
#include "convert/diagnostics.hpp"
#include "convert/market.hpp"
#include "core/dsl/checker.hpp"
#include "core/dsl/linter.hpp"
#include "core/dsl/parser.hpp"
#include "handlers.hpp"

namespace worker {

namespace {

// Ответ по исходнику. Ошибка разбора - единственная диагностика, без
// параметров. Иначе check + lint: уровень - худший по диагностикам check (L001
// на него не влияет, ФТ-13), параметры - из check.
void fillResponse(const std::string &Source,
                  pb::CheckStrategyResponse *Response) {
  const core::dsl::ParseResult Parsed = core::dsl::parse(Source);
  if (!Parsed.Ast) {
    Response->set_level(
        convert::toProto(core::dsl::overallLevel(Parsed.Diagnostics)));
    for (const auto &D : convert::sortedByPosition(Parsed.Diagnostics)) {
      *Response->add_diagnostics() = convert::toProto(D);
    }
    return;
  }

  const core::dsl::CheckResult Checked = core::dsl::check(*Parsed.Ast);
  std::vector<core::dsl::Diagnostic> All = Checked.Diagnostics;
  for (core::dsl::Diagnostic &D : core::dsl::lint(*Parsed.Ast, Source)) {
    All.push_back(std::move(D));
  }

  Response->set_level(
      convert::toProto(core::dsl::overallLevel(Checked.Diagnostics)));
  for (const auto &D : convert::sortedByPosition(std::move(All))) {
    *Response->add_diagnostics() = convert::toProto(D);
  }
  for (const core::dsl::ResolvedParam &P : Checked.Params) {
    *Response->add_params() = convert::toProto(P);
  }
}

} // namespace

// Ни одно исключение не выходит наружу (statusFromCurrentException).
grpc::Status handleCheckStrategy(Context &,
                                 const pb::CheckStrategyRequest &Request,
                                 pb::CheckStrategyResponse *Response) {
  try {
    pb::CheckStrategyResponse Result;
    fillResponse(Request.source(), &Result);
    *Response = std::move(Result);
    return grpc::Status::OK;
  } catch (...) {
    return statusFromCurrentException();
  }
}

} // namespace worker
