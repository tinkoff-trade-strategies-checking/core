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
core::dsl::ParamValues overridesOf(const pb::StrategySource &Strategy) {
  core::dsl::ParamValues Overrides;
  for (const pb::ParamValue &P : Strategy.params()) {
    if (!Overrides.emplace(P.name(), P.value()).second) {
      throw std::invalid_argument("параметр задан дважды: " + P.name());
    }
  }
  return Overrides;
}

// Результат прогона целиком; прогресс идёт в поток по ходу честного прогона.
void run(Context &Context, const pb::RunBacktestRequest &Request,
         grpc::ServerWriterInterface<pb::RunBacktestUpdate> *Writer,
         pb::RunResult *Out) {
  const core::SessionSpec Spec = toSessionSpec(Request.market());
  if (Request.with_cost_breakdown() && Spec.Naive) {
    throw std::invalid_argument("цепочка издержек считается от честного "
                                "прогона: naive = true недопустимо");
  }
  const Strategy Strategy = loadStrategy(Request.strategy().source());
  const core::dsl::ParamValues Overrides = overridesOf(Request.strategy());

  const std::shared_ptr<const core::Timeline> Timeline = loadStressedTimeline(
      Context, Request.market(), Request.stress(), Request.has_stress());

  // Цепочка издержек - ещё шесть прогонов по всей шкале: прогресс идёт на общей
  // шкале из семи частей (честный прогон и шесть этапов), после каждого этапа
  // уходит сообщение, и закрытый поток замечается между этапами.
  const std::int64_t TotalSteps = Timeline->totalSteps();
  constexpr std::int64_t kChainStages = 6;
  const std::int64_t Parts =
      Request.with_cost_breakdown() ? kChainStages + 1 : 1;
  const core::RunOutput Output = core::runBacktest(
      Spec, Timeline, Strategy.Program, Strategy.Params, Overrides,
      Strategy.Severity, progressWriter(Writer, Parts));
  const core::Report Report = core::computeReport(
      metricsInput(Output, Spec.InitialCash, *Timeline,
                   loadBenchmark(Context, Request.market())));

  std::optional<core::CostBreakdown> Breakdown;
  if (Request.with_cost_breakdown()) {
    std::int64_t StagesDone = 0;
    Breakdown = core::costBreakdown(
        Spec, Timeline, Strategy.Program, Strategy.Params, Overrides,
        Report.Stats.Sharpe, [&] {
          writeProgress(Writer, TotalSteps * (1 + ++StagesDone),
                        TotalSteps * Parts);
        });
  }
  toProto(Output, Report, Breakdown, *Timeline, Out);
  // Журнал стратегии - для воспроизведения прогона через ReplaySession
  // (RunResult.journal).
  for (const core::JournalEntry &E : Output.Journal)
    toProto(E, *Timeline, Out->add_journal());
}

} // namespace

// Ни одно исключение не выходит наружу (statusFromCurrentException). Результат
// собирается целиком и пишется в поток последним сообщением; до него идёт
// только прогресс (при цепочке издержек - ещё по сообщению после каждого
// этапа).
grpc::Status
handleRunBacktest(Context &Context, const pb::RunBacktestRequest &Request,
                  grpc::ServerWriterInterface<pb::RunBacktestUpdate> *Writer) {
  try {
    pb::RunBacktestUpdate Update;
    run(Context, Request, Writer, Update.mutable_result());
    if (!Writer->Write(Update))
      return grpc::Status(grpc::StatusCode::CANCELLED, "клиент закрыл поток");
    return grpc::Status::OK;
  } catch (const StreamClosed &) {
    return grpc::Status(grpc::StatusCode::CANCELLED, "клиент закрыл поток");
  } catch (...) {
    return statusFromCurrentException();
  }
}

} // namespace worker
