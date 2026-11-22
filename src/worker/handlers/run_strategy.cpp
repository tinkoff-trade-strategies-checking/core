#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "convert/market.hpp"
#include "convert/run.hpp"
#include "core/report/metrics_input.hpp"
#include "core/strategy/strategy_runner.hpp"
#include "handlers.hpp"

namespace worker {

// Ни одно исключение не выходит наружу (statusFromCurrentException).
grpc::Status handleRunStrategy(
    Context &Ctx, const pb::RunStrategyRequest &Request,
    grpc::ServerWriter<pb::RunStrategyUpdate> *Writer,
    const std::function<bool()> &IsCancelled) {
  try {
    if (!Request.has_market()) {
      throw std::invalid_argument("не задан market");
    }
    if (!Request.has_strategy()) {
      throw std::invalid_argument("не задана strategy");
    }
    if (Writer == nullptr) {
      throw std::invalid_argument("не задан поток ответа");
    }

    const pb::MarketSpec &Market = Request.market();
    const core::SessionSpec Spec = toSessionSpec(Market);
    std::map<std::string, double> Overrides =
        parseParamOverrides(Request.strategy().param_overrides());
    const std::shared_ptr<const core::Timeline> Timeline =
        loadTimeline(Ctx, Market);

    bool StreamAborted = false;
    std::int64_t LastSentDone = -1;

    core::StrategyRunOptions Opts;
    Opts.Source = Request.strategy().source();
    Opts.ParamOverrides = std::move(Overrides);
    Opts.ShouldCancel = [&IsCancelled, &StreamAborted]() {
      return StreamAborted || (IsCancelled && IsCancelled());
    };
    Opts.OnProgress = [Writer, &LastSentDone, &StreamAborted](
                          std::int64_t DoneSteps, std::int64_t TotalSteps) {
      if (StreamAborted) {
        return;
      }
      const std::int64_t MinStep = std::max<std::int64_t>(50, TotalSteps / 20);
      if (DoneSteps < TotalSteps && LastSentDone >= 0 &&
          (DoneSteps - LastSentDone) < MinStep) {
        return;
      }
      pb::RunStrategyUpdate Update;
      Update.mutable_progress()->set_done_steps(DoneSteps);
      Update.mutable_progress()->set_total_steps(TotalSteps);
      if (!Writer->Write(Update)) {
        StreamAborted = true;
        return;
      }
      LastSentDone = DoneSteps;
    };

    const core::StrategyRunResult Run =
        core::runStrategy(Spec, Timeline, Opts);
    if (Run.Cancelled || StreamAborted || (IsCancelled && IsCancelled())) {
      return grpc::Status::CANCELLED;
    }

    std::optional<core::Report> ReportOpt;
    if (Run.Engine.has_value()) {
      const std::int64_t LastStep = Run.Engine->currentStep();
      const std::int32_t UpToYmd = Timeline->yyyymmddOf(LastStep);
      const core::RuoniaSeries Ruonia = loadRuonia(Ctx, Market, UpToYmd);
      const std::int32_t PreviousDay =
          previousTradingDay(Ctx, Market, *Timeline);
      const std::optional<std::int32_t> CompletedDay =
          core::lastCompletedDayYmd(*Timeline, LastStep);
      const std::optional<core::BenchmarkSeries> Benchmark =
          CompletedDay.has_value() ? loadBenchmark(Ctx, Market, *CompletedDay)
                                   : std::nullopt;
      ReportOpt = core::buildStrategyReport(
          Run, Spec.InitialCash, *Timeline, Benchmark, Ruonia, PreviousDay);
    }

    pb::RunStrategyUpdate FinalUpdate;
    toProto(Run, ReportOpt ? &*ReportOpt : nullptr, *Timeline,
            Request.include_history(), FinalUpdate.mutable_result());
    if (!Writer->Write(FinalUpdate)) {
      return grpc::Status::CANCELLED;
    }
    return grpc::Status::OK;
  } catch (...) {
    return statusFromCurrentException();
  }
}

} // namespace worker
