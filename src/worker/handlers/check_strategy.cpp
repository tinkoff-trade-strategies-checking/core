#include <stdexcept>
#include <utility>
#include <vector>

#include "clickhouse/clickhouse_client.hpp"
#include "convert/diagnostics.hpp"
#include "core/dsl/checker.hpp"
#include "core/dsl/linter.hpp"
#include "core/dsl/parser.hpp"
#include "handlers.hpp"

namespace worker {

namespace {

// Ответ по исходнику. Ошибка разбора - единственная диагностика, без параметров. Иначе check + lint:
// уровень - худший по диагностикам check (L001 на него не влияет, ФТ-13), параметры - из check.
void fillResponse(const std::string& source, pb::CheckStrategyResponse* response) {
  const core::dsl::ParseResult parsed = core::dsl::parse(source);
  if (!parsed.program) {
    response->set_level(convert::toProto(core::dsl::overallLevel(parsed.diagnostics)));
    for (const auto& d : convert::sortedByPosition(parsed.diagnostics)) {
      *response->add_diagnostics() = convert::toProto(d);
    }
    return;
  }

  const core::dsl::CheckResult checked = core::dsl::check(*parsed.program);
  std::vector<core::dsl::Diagnostic> all = checked.diagnostics;
  for (core::dsl::Diagnostic& d : core::dsl::lint(*parsed.program, source)) {
    all.push_back(std::move(d));
  }

  response->set_level(convert::toProto(core::dsl::overallLevel(checked.diagnostics)));
  for (const auto& d : convert::sortedByPosition(std::move(all))) {
    *response->add_diagnostics() = convert::toProto(d);
  }
  for (const core::dsl::ResolvedParam& p : checked.params) {
    *response->add_params() = convert::toProto(p);
  }
}

}  // namespace

// Ни одно исключение не выходит наружу. ClickHouseError - потомок MarketDataError, поэтому первым.
grpc::Status handleCheckStrategy(Context&, const pb::CheckStrategyRequest& request,
                                 pb::CheckStrategyResponse* response) {
  try {
    pb::CheckStrategyResponse result;
    fillResponse(request.source(), &result);
    *response = std::move(result);
    return grpc::Status::OK;
  } catch (const ClickHouseError&) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "хранилище данных недоступно");
  } catch (const core::MarketDataError& e) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, e.what());
  } catch (const std::invalid_argument& e) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what());
  } catch (...) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "внутренняя ошибка воркера");
  }
}

}  // namespace worker
