#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "convert/market.hpp"
#include "convert/progress.hpp"
#include "convert/report.hpp"
#include "convert/run.hpp"
#include "convert/state.hpp"
#include "convert/strategy.hpp"
#include "core/analysis/cost_breakdown.hpp"
#include "core/money/proto_conv.hpp"
#include "core/report/metrics.hpp"
#include "core/run/backtest.hpp"
#include "handlers.hpp"

namespace worker {

namespace {

// Значения параметров на прогон. Диапазон и имена проверяет StrategyInstance
// (invalid_argument); здесь - только повтор.
core::dsl::ParamValues overridesOf(const pb::StrategySource &strategy) {
  core::dsl::ParamValues overrides;
  for (const pb::ParamValue &p : strategy.params()) {
    if (!overrides.emplace(p.name(), p.value()).second) {
      throw std::invalid_argument("параметр задан дважды: " + p.name());
    }
  }
  return overrides;
}

// Результат прогона целиком; прогресс идёт в поток по ходу честного прогона.
void run(Context &context, const pb::RunBacktestRequest &request,
         grpc::ServerWriterInterface<pb::RunBacktestUpdate> *writer,
         pb::RunResult *out) {
  const core::SessionSpec spec = toSessionSpec(request.market());
  if (request.with_cost_breakdown() && spec.naive) {
    throw std::invalid_argument("цепочка издержек считается от честного "
                                "прогона: naive = true недопустимо");
  }
  const Strategy strategy = loadStrategy(request.strategy().source());
  const core::dsl::ParamValues overrides = overridesOf(request.strategy());

  const std::shared_ptr<const core::Timeline> timeline = loadStressedTimeline(
      context, request.market(), request.stress(), request.has_stress());

  // Цепочка издержек - ещё шесть прогонов по всей шкале: прогресс идёт на общей
  // шкале из семи частей (честный прогон и шесть этапов), после каждого этапа
  // уходит сообщение, и закрытый поток замечается между этапами.
  const std::int64_t totalSteps = timeline->totalSteps();
  constexpr std::int64_t kChainStages = 6;
  const std::int64_t parts =
      request.with_cost_breakdown() ? kChainStages + 1 : 1;
  const core::RunOutput output = core::runBacktest(
      spec, timeline, strategy.program, strategy.params, overrides,
      strategy.level, progressWriter(writer, parts));
  const core::Report report = core::computeReport(
      metricsInput(output, spec.initialCash, *timeline,
                   loadBenchmark(context, request.market())));

  std::optional<core::CostBreakdown> breakdown;
  if (request.with_cost_breakdown()) {
    std::int64_t stagesDone = 0;
    breakdown = core::costBreakdown(
        spec, timeline, strategy.program, strategy.params, overrides,
        report.metrics.sharpe, [&] {
          writeProgress(writer, totalSteps * (1 + ++stagesDone),
                        totalSteps * parts);
        });
  }
  toProto(output, report, breakdown, *timeline, out);
  // Журнал стратегии - для воспроизведения прогона через ReplaySession
  // (RunResult.journal).
  for (const core::JournalEntry &e : output.journal)
    toProto(e, *timeline, out->add_journal());
}

} // namespace

// Ни одно исключение не выходит наружу (statusFromCurrentException). Результат
// собирается целиком и пишется в поток последним сообщением; до него идёт
// только прогресс (при цепочке издержек - ещё по сообщению после каждого
// этапа).
grpc::Status
handleRunBacktest(Context &context, const pb::RunBacktestRequest &request,
                  grpc::ServerWriterInterface<pb::RunBacktestUpdate> *writer) {
  try {
    pb::RunBacktestUpdate update;
    run(context, request, writer, update.mutable_result());
    if (!writer->Write(update))
      return grpc::Status(grpc::StatusCode::CANCELLED, "клиент закрыл поток");
    return grpc::Status::OK;
  } catch (const StreamClosed &) {
    return grpc::Status(grpc::StatusCode::CANCELLED, "клиент закрыл поток");
  } catch (...) {
    return statusFromCurrentException();
  }
}

} // namespace worker
