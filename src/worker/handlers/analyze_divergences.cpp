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
core::AlgorithmInput toAlgorithm(const pb::AlgorithmRef &Ref) {
  core::dsl::ParseResult Parsed = core::dsl::parse(Ref.strategy().source());
  if (!Parsed.Ast)
    throw std::invalid_argument("алгоритм " + Ref.id() +
                                ": ошибка разбора исходника");
  core::dsl::CheckResult Checked = core::dsl::check(*Parsed.Ast);
  if (Checked.Severity == core::dsl::Level::Error) {
    throw std::invalid_argument("алгоритм " + Ref.id() +
                                ": стратегия не прошла проверку");
  }
  core::dsl::ParamValues Values;
  for (const pb::ParamValue &P : Ref.strategy().params())
    Values[P.name()] = P.value();
  return core::AlgorithmInput{Ref.id(), std::move(*Parsed.Ast),
                              std::move(Checked.Params), std::move(Values)};
}

// Бенчмарк независимых портфелей по шагам 0…lastStep (как в ReplaySession): бар
// D1 дня, в котором сессия остановилась посреди дня, содержит будущее (INV-1),
// поэтому период бенчмарка заканчивается последним полностью пройденным днём.
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

pb::DivergenceType divergenceTypeToProto(core::DivergenceType Type) {
  switch (Type) {
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

void fill(Context &Context, const pb::DivergenceRequest &Request,
          pb::DivergenceResponse *Out) {
  const pb::MarketSpec &Market = Request.market();
  const core::SessionSpec Spec = toSessionSpec(Market);
  const std::shared_ptr<const core::Timeline> Timeline =
      loadTimeline(Context, Market);

  std::vector<core::JournalEntry> Journal;
  Journal.reserve(static_cast<std::size_t>(Request.journal_size()));
  for (const pb::JournalEntry &E : Request.journal())
    Journal.push_back(fromProto(E, Spec));
  std::vector<core::AlgorithmInput> Algorithms;
  Algorithms.reserve(static_cast<std::size_t>(Request.algorithms_size()));
  for (const pb::AlgorithmRef &Ref : Request.algorithms())
    Algorithms.push_back(toAlgorithm(Ref));

  const core::DivergenceResult Result = core::analyzeDivergences(
      Spec, Timeline, Journal, Request.last_step(), Algorithms);

  const auto KeyOf = [&](int Instrument) {
    return Timeline->instruments()[static_cast<std::size_t>(Instrument)].Key;
  };
  for (const core::Divergence &D : Result.Divergences) {
    pb::Divergence *P = Out->add_divergences();
    P->set_algorithm_id(D.AlgorithmId);
    P->set_instrument_key(KeyOf(D.Instrument));
    P->set_type(divergenceTypeToProto(D.Type));
    P->set_human_step(D.HumanStep);
    P->set_algo_step(D.AlgoStep);
    P->set_human_lots(D.HumanLots);
    P->set_algo_lots(D.AlgoLots);
    P->set_horizon_step(D.HorizonStep);
    core::toProto(D.Cost, P->mutable_cost());
    P->set_cost_percent(D.CostPercent);
    P->set_reason(D.ReasonCode);
  }
  for (const core::MissedProfitableTrade &M : Result.Missed) {
    pb::MissedProfitableTrade *P = Out->add_missed_profitable();
    P->set_instrument_key(KeyOf(M.Instrument));
    P->set_entry_step(M.EntryStep);
    P->set_exit_step(M.ExitStep);
    P->set_lots(M.Lots);
    core::toProto(M.NetPnl, P->mutable_net_pnl());
    for (const std::string &Id : M.AlgorithmIds)
      P->add_algorithm_ids(Id);
    core::toProto(M.EntryPrice, P->mutable_entry_price());
    core::toProto(M.ExitPrice, P->mutable_exit_price());
  }
  for (const core::ConsensusItem &C : Result.Consensus) {
    pb::ConsensusItem *P = Out->add_consensus();
    P->set_instrument_key(KeyOf(C.Instrument));
    P->set_step(C.Step);
    P->set_side(toProto(C.OrderSide));
    P->set_human_action(C.HumanAction);
    P->set_agreeing(C.Agreeing);
    P->set_total(C.Total);
    core::toProto(C.MedianCost, P->mutable_median_cost());
  }
  // Независимые портфели - по одному на алгоритм запроса, в его порядке
  // (ФТ-18).
  if (!Result.Independent.empty()) {
    const auto Benchmark =
        benchmarkUpTo(Context, Market, *Timeline, Request.last_step());
    for (const core::RunOutput &Run : Result.Independent) {
      toProto(Run,
              core::computeReport(
                  metricsInput(Run, Spec.InitialCash, *Timeline, Benchmark)),
              std::nullopt, *Timeline, Out->add_algorithm_portfolios());
    }
  }
}

} // namespace

// Ни одно исключение не выходит наружу (statusFromCurrentException). Ответ
// собирается отдельно и записывается в response только при успехе.
grpc::Status handleAnalyzeDivergences(Context &Context,
                                      const pb::DivergenceRequest &Request,
                                      pb::DivergenceResponse *Response) {
  try {
    pb::DivergenceResponse Result;
    fill(Context, Request, &Result);
    *Response = std::move(Result);
    return grpc::Status::OK;
  } catch (...) {
    return statusFromCurrentException();
  }
}

} // namespace worker
