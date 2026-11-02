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

// События (events_from_step; up_to_step] в контракте, а replaySession берёт шаги «от» включительно.
// Отрицательное значение - события не нужны: «от» за границей любого шага.
std::int64_t firstEventStep(std::int64_t eventsFromStep) {
  if (eventsFromStep < 0) return std::numeric_limits<std::int64_t>::max();
  return eventsFromStep + 1;
}

// Бенчмарк для отчёта по шагам 0…lastStep. Бар D1 дня, в котором сессия остановилась посреди дня, содержит
// будущее (INV-1), поэтому период бенчмарка заканчивается последним полностью пройденным днём.
std::optional<std::pair<core::Decimal, core::Decimal>> benchmarkUpTo(Context& context, const pb::MarketSpec& market,
                                                                     const core::Timeline& timeline,
                                                                     std::int64_t lastStep) {
  std::int64_t step = lastStep;
  if (!timeline.isLastStepOfDay(step)) {
    const std::int32_t today = timeline.yyyymmddOf(step);
    while (step >= 0 && timeline.yyyymmddOf(step) == today) --step;
    if (step < 0) return std::nullopt;  // ни одного полного дня
  }
  pb::MarketSpec limited = market;
  limited.set_to_date(formatIsoDate(timeline.yyyymmddOf(step)));
  return loadBenchmark(context, limited);
}

void fill(Context& context, const pb::ReplaySessionRequest& request, pb::ReplaySessionResponse* out) {
  const pb::MarketSpec& market = request.market();
  const core::SessionSpec spec = toSessionSpec(market);
  const std::shared_ptr<const core::Timeline> timeline = loadTimeline(context, market);

  if (request.up_to_step() < 0 || request.up_to_step() >= timeline->totalSteps()) {
    throw std::invalid_argument("up_to_step вне шкалы времени сессии: " + std::to_string(request.up_to_step()) +
                                ", шагов " + std::to_string(timeline->totalSteps()));
  }

  std::vector<core::JournalEntry> journal;
  journal.reserve(static_cast<std::size_t>(request.journal_size()));
  for (const pb::JournalEntry& e : request.journal()) journal.push_back(fromProto(e, spec));

  const core::ReplayResult result = core::replaySession(spec, timeline, journal, request.up_to_step(),
                                                        firstEventStep(request.events_from_step()));
  const core::SessionEngine& engine = result.engine;

  out->set_total_steps(timeline->totalSteps());
  out->set_status(toProto(result.status));
  out->set_last_step(result.lastStep);
  for (const core::StepEvents& e : result.events) toProto(e, *timeline, out->add_events());
  if (request.include_history()) {
    // До last_step, а не до up_to_step: сессия могла остановиться раньше (банкротство, INV-1).
    for (std::int64_t step = 0; step <= result.lastStep; ++step) {
      for (const core::StepBar& b : timeline->barsAt(step)) toProto(b, *timeline, out->add_history());
    }
  }
  toProto(engine.snapshot(), *timeline, out->mutable_portfolio());
  for (const core::Order& o : engine.orders()) toProto(o, *timeline, out->add_orders());
  for (const core::Fill& f : engine.fills()) toProto(f, *timeline, out->add_fills());
  for (const core::CommandResult& r : result.commandResults) toProto(r, out->add_command_results());
  if (request.include_report()) {
    toProto(core::computeReport(metricsInput(engine, spec.initialCash, *timeline,
                                              benchmarkUpTo(context, market, *timeline, result.lastStep))),
            out->mutable_report());
  }
}

}  // namespace

// Ни одно исключение не выходит наружу (statusFromCurrentException). Ответ собирается отдельно и
// записывается в response только при успехе.
grpc::Status handleReplaySession(Context& context, const pb::ReplaySessionRequest& request,
                                 pb::ReplaySessionResponse* response) {
  try {
    pb::ReplaySessionResponse result;
    fill(context, request, &result);
    *response = std::move(result);
    return grpc::Status::OK;
  } catch (...) {
    return statusFromCurrentException();
  }
}

}  // namespace worker
