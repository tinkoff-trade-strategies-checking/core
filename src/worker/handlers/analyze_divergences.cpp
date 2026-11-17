#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "convert/market.hpp"
#include "convert/report.hpp"
#include "convert/run.hpp"
#include "convert/state.hpp"
#include "core/analysis/divergence.hpp"
#include "core/dsl/checker.hpp"
#include "core/dsl/parser.hpp"
#include "core/money/proto_conv.hpp"
#include "core/report/metrics.hpp"
#include "handlers.hpp"

namespace worker {

namespace {

// Алгоритм из запроса: разбор и проверка (INV-1: стратегия с ошибкой в разбор
// не идёт), значения параметров - как есть, диапазоны проверит StrategyInstance
// при создании.
core::AlgorithmInput toAlgorithm(const pb::AlgorithmRef &ref) {
  core::dsl::ParseResult parsed = core::dsl::parse(ref.strategy().source());
  if (!parsed.program)
    throw std::invalid_argument("алгоритм " + ref.id() +
                                ": ошибка разбора исходника");
  core::dsl::CheckResult checked = core::dsl::check(*parsed.program);
  if (checked.level == core::dsl::Level::Error) {
    throw std::invalid_argument("алгоритм " + ref.id() +
                                ": стратегия не прошла проверку");
  }
  core::dsl::ParamValues values;
  for (const pb::ParamValue &p : ref.strategy().params())
    values[p.name()] = p.value();
  return core::AlgorithmInput{ref.id(), std::move(*parsed.program),
                              std::move(checked.params), std::move(values)};
}

// Бенчмарк независимых портфелей по шагам 0…lastStep (как в ReplaySession): бар
// D1 дня, в котором сессия остановилась посреди дня, содержит будущее (INV-1),
// поэтому период бенчмарка заканчивается последним полностью пройденным днём.
std::optional<core::BenchmarkSeries>
benchmarkUpTo(Context &context, const pb::MarketSpec &market,
              const core::Timeline &timeline, std::int64_t lastStep) {
  std::int64_t step = lastStep;
  if (!timeline.isLastStepOfDay(step)) {
    const std::int32_t today = timeline.yyyymmddOf(step);
    while (step >= 0 && timeline.yyyymmddOf(step) == today)
      --step;
    if (step < 0)
      return std::nullopt; // ни одного полного дня
  }
  pb::MarketSpec limited = market;
  limited.set_to_date(formatIsoDate(timeline.yyyymmddOf(step)));
  return loadBenchmark(context, limited);
}

pb::DivergenceType divergenceTypeToProto(core::DivergenceType type) {
  switch (type) {
  case core::DivergenceType::Excess:
    return pb::EXCESS;
  case core::DivergenceType::Missed:
    return pb::MISSED;
  case core::DivergenceType::Size:
    return pb::SIZE;
  case core::DivergenceType::Timing:
    return pb::TIMING;
  }
  return pb::DIVERGENCE_TYPE_UNSPECIFIED;
}

void fill(Context &context, const pb::DivergenceRequest &request,
          pb::DivergenceResponse *out) {
  const pb::MarketSpec &market = request.market();
  const core::SessionSpec spec = toSessionSpec(market);
  const std::shared_ptr<const core::Timeline> timeline =
      loadTimeline(context, market);

  std::vector<core::JournalEntry> journal;
  journal.reserve(static_cast<std::size_t>(request.journal_size()));
  for (const pb::JournalEntry &e : request.journal())
    journal.push_back(fromProto(e, spec));
  std::vector<core::AlgorithmInput> algorithms;
  algorithms.reserve(static_cast<std::size_t>(request.algorithms_size()));
  for (const pb::AlgorithmRef &ref : request.algorithms())
    algorithms.push_back(toAlgorithm(ref));

  const core::DivergenceResult result = core::analyzeDivergences(
      spec, timeline, journal, request.last_step(), algorithms);

  const auto keyOf = [&](int instrument) {
    return timeline->instruments()[static_cast<std::size_t>(instrument)].key;
  };
  for (const core::Divergence &d : result.divergences) {
    pb::Divergence *p = out->add_divergences();
    p->set_algorithm_id(d.algorithmId);
    p->set_instrument_key(keyOf(d.instrument));
    p->set_type(divergenceTypeToProto(d.type));
    p->set_human_step(d.humanStep);
    p->set_algo_step(d.algoStep);
    p->set_human_lots(d.humanLots);
    p->set_algo_lots(d.algoLots);
    p->set_horizon_step(d.horizonStep);
    core::toProto(d.cost, p->mutable_cost());
    p->set_cost_percent(d.costPercent);
    p->set_reason(d.reason);
  }
  for (const core::MissedProfitableTrade &m : result.missed) {
    pb::MissedProfitableTrade *p = out->add_missed_profitable();
    p->set_instrument_key(keyOf(m.instrument));
    p->set_entry_step(m.entryStep);
    p->set_exit_step(m.exitStep);
    p->set_lots(m.lots);
    core::toProto(m.netPnl, p->mutable_net_pnl());
    for (const std::string &id : m.algorithmIds)
      p->add_algorithm_ids(id);
    core::toProto(m.entryPrice, p->mutable_entry_price());
    core::toProto(m.exitPrice, p->mutable_exit_price());
  }
  for (const core::ConsensusItem &c : result.consensus) {
    pb::ConsensusItem *p = out->add_consensus();
    p->set_instrument_key(keyOf(c.instrument));
    p->set_step(c.step);
    p->set_side(toProto(c.side));
    p->set_human_action(c.humanAction);
    p->set_agreeing(c.agreeing);
    p->set_total(c.total);
    core::toProto(c.medianCost, p->mutable_median_cost());
  }
  // Независимые портфели - по одному на алгоритм запроса, в его порядке
  // (ФТ-18).
  if (!result.independent.empty()) {
    const auto benchmark =
        benchmarkUpTo(context, market, *timeline, request.last_step());
    for (const core::RunOutput &run : result.independent) {
      toProto(run,
              core::computeReport(
                  metricsInput(run, spec.initialCash, *timeline, benchmark)),
              std::nullopt, *timeline, out->add_algorithm_portfolios());
    }
  }
}

} // namespace

// Ни одно исключение не выходит наружу (statusFromCurrentException). Ответ
// собирается отдельно и записывается в response только при успехе.
grpc::Status handleAnalyzeDivergences(Context &context,
                                      const pb::DivergenceRequest &request,
                                      pb::DivergenceResponse *response) {
  try {
    pb::DivergenceResponse result;
    fill(context, request, &result);
    *response = std::move(result);
    return grpc::Status::OK;
  } catch (...) {
    return statusFromCurrentException();
  }
}

} // namespace worker
