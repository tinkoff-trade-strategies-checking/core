#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "convert/market.hpp"
#include "convert/report.hpp"
#include "convert/state.hpp"
#include "core/engine/session_engine.hpp"
#include "core/report/metrics.hpp"
#include "handlers.hpp"

namespace worker {

namespace {

// События (events_from_step; up_to_step] в контракте, а replaySession берёт
// шаги «от» включительно. Отрицательное значение - события не нужны: «от» за
// границей любого шага.
std::int64_t firstEventStep(std::int64_t EventsFromStep) {
  if (EventsFromStep < 0)
    return std::numeric_limits<std::int64_t>::max();
  return EventsFromStep + 1;
}

// Бенчмарк для отчёта по шагам 0…lastStep. Бар D1 дня, в котором сессия
// остановилась посреди дня, содержит будущее (INV-1), поэтому период бенчмарка
// заканчивается последним полностью пройденным днём.
std::optional<core::BenchmarkSeries>
benchmarkUpTo(Context &Context, const pb::MarketSpec &Market,
              const core::Timeline &Timeline, std::int64_t LastStep) {
  std::int64_t Step = LastStep;
  if (!Timeline.isLastStepOfDay(Step)) {
    const std::int32_t Today = Timeline.yyyymmddOf(Step);
    while (Step >= 0 && Timeline.yyyymmddOf(Step) == Today)
      --Step;
    if (Step < 0)
      return std::nullopt; // ни одного полного дня
  }
  pb::MarketSpec Limited = Market;
  Limited.set_to_date(formatIsoDate(Timeline.yyyymmddOf(Step)));
  return loadBenchmark(Context, Limited);
}

void fill(Context &Context, const pb::ReplaySessionRequest &Request,
          pb::ReplaySessionResponse *Out) {
  const pb::MarketSpec &Market = Request.market();
  const core::SessionSpec Spec = toSessionSpec(Market);
  const std::shared_ptr<const core::Timeline> Timeline =
      loadTimeline(Context, Market);

  if (Request.up_to_step() < 0 ||
      Request.up_to_step() >= Timeline->totalSteps()) {
    throw std::invalid_argument("up_to_step вне шкалы времени сессии: " +
                                std::to_string(Request.up_to_step()) +
                                ", шагов " +
                                std::to_string(Timeline->totalSteps()));
  }

  std::vector<core::JournalEntry> Journal;
  Journal.reserve(static_cast<std::size_t>(Request.journal_size()));
  for (const pb::JournalEntry &E : Request.journal())
    Journal.push_back(fromProto(E, Spec));

  const core::ReplayResult Result =
      core::replaySession(Spec, Timeline, Journal, Request.up_to_step(),
                          firstEventStep(Request.events_from_step()));
  const core::SessionEngine &Engine = Result.Engine;

  Out->set_total_steps(Timeline->totalSteps());
  Out->set_warmup_steps(Timeline->warmupSteps());
  Out->set_status(toProto(Result.Status));
  Out->set_last_step(Result.LastStep);
  for (const core::StepEvents &E : Result.Events)
    toProto(E, *Timeline, Out->add_events());
  if (Request.include_history()) {
    // Прогрев и шаги до last_step, а не до up_to_step: сессия могла
    // остановиться раньше (банкротство, INV-1).
    appendBars(*Timeline, -Timeline->warmupSteps(), Result.LastStep,
               Out->mutable_history());
  }
  toProto(Engine.snapshot(), *Timeline, Out->mutable_portfolio());
  for (const core::Order &O : Engine.orders())
    toProto(O, *Timeline, Out->add_orders());
  for (const core::Fill &F : Engine.fills())
    toProto(F, *Timeline, Out->add_fills());
  for (const core::CommandResult &R : Result.CommandResults)
    toProto(R, Out->add_command_results());
  if (Request.include_report()) {
    toProto(core::computeReport(metricsInput(
                Engine, Spec.InitialCash, *Timeline,
                benchmarkUpTo(Context, Market, *Timeline, Result.LastStep))),
            Out->mutable_report());
  }
}

} // namespace

// Ни одно исключение не выходит наружу (statusFromCurrentException). Ответ
// собирается отдельно и записывается в response только при успехе.
grpc::Status handleReplaySession(Context &Context,
                                 const pb::ReplaySessionRequest &Request,
                                 pb::ReplaySessionResponse *Response) {
  try {
    pb::ReplaySessionResponse Result;
    fill(Context, Request, &Result);
    *Response = std::move(Result);
    return grpc::Status::OK;
  } catch (...) {
    return statusFromCurrentException();
  }
}

} // namespace worker
