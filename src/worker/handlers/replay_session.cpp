#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "convert/blind.hpp"
#include "convert/market.hpp"
#include "convert/report.hpp"
#include "convert/state.hpp"
#include "core/engine/session_engine.hpp"
#include "core/money/proto_conv.hpp"
#include "core/report/derived.hpp"
#include "core/report/metrics.hpp"
#include "core/report/metrics_input.hpp"
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
  const std::optional<std::int32_t> CompletedDay =
      core::lastCompletedDayYmd(Timeline, LastStep);
  if (!CompletedDay)
    return std::nullopt; // ни одного полного дня
  // Ряд читается (и кэшируется) за весь период сессии и обрезается по дню
  // курсора после чтения.
  return loadBenchmark(Context, Market, *CompletedDay);
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
  if (Result.LastStep >= 0 && Result.LastStep < Timeline->totalSteps())
    Out->set_current_day(Timeline->tradingDayOf(Result.LastStep));
  const core::CostLedger Ledger(Engine.fills());
  for (const core::StepEvents &E : Result.Events)
    toProto(E, *Timeline, Spec.InitialCash, Ledger, Out->add_events());
  if (Request.include_history()) {
    // Прогрев и шаги до last_step, а не до up_to_step: сессия могла
    // остановиться раньше (банкротство, INV-1).
    appendBars(*Timeline, -Timeline->warmupSteps(), Result.LastStep,
               Out->mutable_history());
  }
  toProto(Engine.snapshot(), *Timeline, Spec.InitialCash,
          Ledger.upTo(Result.LastStep), Out->mutable_portfolio());
  for (std::size_t I = 0; I < Timeline->instruments().size(); ++I) {
    pb::InstrumentChange *Change = Out->add_instrument_changes();
    Change->set_instrument_key(Timeline->instrumentKey(static_cast<int>(I)));
    const core::InstrumentChange C =
        core::instrumentChange(*Timeline, static_cast<int>(I), Result.LastStep);
    if (C.StepPct)
      core::toProto(*C.StepPct, Change->mutable_step_pct());
    if (C.PeriodPct)
      core::toProto(*C.PeriodPct, Change->mutable_period_pct());
  }
  for (const core::Order &O : Engine.orders())
    toProto(O, *Timeline, Out->add_orders());
  for (const core::Fill &F : Engine.fills())
    toProto(F, *Timeline, Spec.InitialCash, Out->add_fills());
  for (const core::CommandResult &R : Result.CommandResults)
    toProto(R, Out->add_command_results());
  std::optional<core::Report> CoreReport;
  if (Request.include_report()) {
    const std::int32_t UpToYmd = Timeline->yyyymmddOf(Result.LastStep);
    const core::RuoniaSeries Ruonia = loadRuonia(Context, Market, UpToYmd);
    const std::int32_t PreviousDay =
        previousTradingDay(Context, Market, *Timeline);
    CoreReport = core::computeReport(
        metricsInput(Engine, Spec.InitialCash, *Timeline,
                     benchmarkUpTo(Context, Market, *Timeline, Result.LastStep),
                     Ruonia, PreviousDay));
    toProto(*CoreReport, *Timeline, Out->mutable_report());
  }
  if (Request.blind()) {
    fillBlindView(*Timeline, Spec.InitialCash, Result.LastStep,
                  Request.include_history(), Result.Events, Engine, CoreReport,
                  Out->mutable_blind_view());
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
