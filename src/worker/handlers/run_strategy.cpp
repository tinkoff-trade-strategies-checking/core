#include <algorithm>
#include <chrono>
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
grpc::Status handleRunStrategyWithSink(
    Context &Ctx, const pb::RunStrategyRequest &Request,
    const std::function<bool(const pb::RunStrategyUpdate &)> &WriteUpdate,
    const std::function<bool()> &IsCancelled,
    std::optional<std::chrono::steady_clock::duration> TimeBudget) {
  try {
    if (!Request.has_market()) {
      throw std::invalid_argument("не задан market");
    }
    if (!Request.has_strategy()) {
      throw std::invalid_argument("не задана strategy");
    }
    if (!WriteUpdate) {
      throw std::invalid_argument("не задан поток ответа");
    }

    const auto StartSteady = std::chrono::steady_clock::now();
    bool ClientCancelled = false;
    const auto CheckClientCancelled = [&IsCancelled, &ClientCancelled]() {
      if (ClientCancelled) {
        return true;
      }
      if (IsCancelled && IsCancelled()) {
        ClientCancelled = true;
        return true;
      }
      return false;
    };

    if (CheckClientCancelled()) {
      return grpc::Status::CANCELLED;
    }
    if (TimeBudget.has_value() &&
        *TimeBudget <= std::chrono::steady_clock::duration::zero()) {
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                          kTimeBudgetExceededMessage);
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
    Opts.ShouldCancel = [&CheckClientCancelled, &StreamAborted]() {
      return StreamAborted || CheckClientCancelled();
    };
    if (TimeBudget.has_value()) {
      const auto Elapsed = std::chrono::steady_clock::now() - StartSteady;
      if (Elapsed >= *TimeBudget) {
        return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                            kTimeBudgetExceededMessage);
      }
      Opts.TimeBudget = *TimeBudget - Elapsed;
    }
    Opts.OnProgress = [&WriteUpdate, &LastSentDone, &StreamAborted](
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
      if (!WriteUpdate(Update)) {
        StreamAborted = true;
        return;
      }
      LastSentDone = DoneSteps;
    };

    const core::StrategyRunResult Run = core::runStrategy(Spec, Timeline, Opts);
    if (StreamAborted || CheckClientCancelled()) {
      return grpc::Status::CANCELLED;
    }
    if (Run.Cancelled) {
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                          kTimeBudgetExceededMessage);
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
      ReportOpt = core::buildStrategyReport(Run, Spec.InitialCash, *Timeline,
                                            Benchmark, Ruonia, PreviousDay);
    }

    pb::RunStrategyUpdate FinalUpdate;
    toProto(Run, ReportOpt ? &*ReportOpt : nullptr, *Timeline,
            Request.include_history(), FinalUpdate.mutable_result());
    if (!WriteUpdate(FinalUpdate)) {
      return grpc::Status::CANCELLED;
    }
    return grpc::Status::OK;
  } catch (...) {
    return statusFromCurrentException();
  }
}

grpc::Status handleRunStrategy(
    Context &Ctx, const pb::RunStrategyRequest &Request,
    grpc::ServerWriter<pb::RunStrategyUpdate> *Writer,
    const std::function<bool()> &IsCancelled,
    std::optional<std::chrono::steady_clock::duration> TimeBudget) {
  if (Writer == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "не задан поток ответа");
  }
  return handleRunStrategyWithSink(
      Ctx, Request,
      [Writer](const pb::RunStrategyUpdate &Update) {
        return Writer->Write(Update);
      },
      IsCancelled, TimeBudget);
}

} // namespace worker
