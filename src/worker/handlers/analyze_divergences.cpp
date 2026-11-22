#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "convert/market.hpp"
#include "convert/run.hpp"
#include "convert/state.hpp"
#include "core/analysis/divergence.hpp"
#include "core/money/proto_conv.hpp"
#include "core/report/benchmark.hpp"
#include "core/report/metrics_input.hpp"
#include "core/report/ruonia.hpp"
#include "handlers.hpp"

namespace worker {

namespace {

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

pb::OrderType toOptionalOrderType(std::optional<core::OrderType> Type) {
  return Type.has_value() ? worker::toProto(*Type) : pb::ORDER_TYPE_UNSPECIFIED;
}

void writeDivergence(const core::Divergence &Src,
                     const core::Timeline &Timeline, pb::Divergence *Out) {
  Out->set_algorithm_id(Src.AlgorithmId);
  Out->set_instrument_key(Timeline.instrumentKey(Src.Instrument));
  Out->set_type(divergenceTypeToProto(Src.Type));
  Out->set_human_step(Src.HumanStep);
  Out->set_algo_step(Src.AlgoStep);
  Out->set_human_lots(Src.HumanLots);
  Out->set_algo_lots(Src.AlgoLots);
  Out->set_horizon_step(Src.HorizonStep);
  core::toProto(Src.Cost, Out->mutable_cost());
  core::toProto(Src.CostPercent, Out->mutable_cost_percent());
  Out->set_reason(Src.ReasonCode);
  Out->set_seq(Src.Seq);
  Out->set_human_order_type(toOptionalOrderType(Src.HumanOrderType));
  Out->set_algo_order_type(toOptionalOrderType(Src.AlgoOrderType));
}

void writeMissed(const core::MissedProfitableTrade &Src,
                 const core::Timeline &Timeline,
                 pb::MissedProfitableTrade *Out) {
  Out->set_instrument_key(Timeline.instrumentKey(Src.Instrument));
  Out->set_entry_step(Src.EntryStep);
  Out->set_exit_step(Src.ExitStep);
  Out->set_lots(Src.Lots);
  core::toProto(Src.NetPnl, Out->mutable_net_pnl());
  for (const std::string &Id : Src.AlgorithmIds) {
    Out->add_algorithm_ids(Id);
  }
  core::toProto(Src.EntryPrice, Out->mutable_entry_price());
  core::toProto(Src.ExitPrice, Out->mutable_exit_price());
  Out->set_seq(Src.Seq);
  for (const std::int64_t DivSeq : Src.DivergenceSeqs) {
    Out->add_divergence_seqs(DivSeq);
  }
}

void writeConsensus(const core::ConsensusItem &Src,
                    const core::Timeline &Timeline, pb::ConsensusItem *Out) {
  Out->set_instrument_key(Timeline.instrumentKey(Src.Instrument));
  Out->set_step(Src.Step);
  Out->set_side(toProto(Src.OrderSide));
  Out->set_human_action(Src.HumanAction);
  Out->set_agreeing(Src.Agreeing);
  Out->set_total(Src.Total);
  core::toProto(Src.MedianCost, Out->mutable_median_cost());
}

void fill(Context &Ctx, const pb::DivergenceRequest &Request,
          pb::DivergenceResponse *Out) {
  if (!Request.has_market()) {
    throw std::invalid_argument("не задан market");
  }
  const pb::MarketSpec &Market = Request.market();
  const core::SessionSpec Spec = toSessionSpec(Market);
  const std::shared_ptr<const core::Timeline> Timeline =
      loadTimeline(Ctx, Market);

  if (Request.last_step() < 0 ||
      Request.last_step() >= Timeline->totalSteps()) {
    throw std::invalid_argument("last_step вне шкалы времени сессии: " +
                                std::to_string(Request.last_step()) +
                                ", шагов " +
                                std::to_string(Timeline->totalSteps()));
  }

  std::vector<core::JournalEntry> Journal;
  Journal.reserve(static_cast<std::size_t>(Request.journal_size()));
  for (const pb::JournalEntry &Entry : Request.journal()) {
    Journal.push_back(fromProto(Entry, Spec));
  }

  std::vector<core::AlgorithmInput> Algorithms;
  Algorithms.reserve(static_cast<std::size_t>(Request.algorithms_size()));
  for (const pb::AlgorithmRef &Alg : Request.algorithms()) {
    if (!Alg.has_strategy()) {
      throw std::invalid_argument("не задана strategy для алгоритма: " +
                                  Alg.id());
    }
    core::AlgorithmInput Input;
    Input.Id = Alg.id();
    Input.Source = Alg.strategy().source();
    Input.ParamOverrides =
        parseParamOverrides(Alg.strategy().param_overrides());
    Algorithms.push_back(std::move(Input));
  }

  std::optional<core::BenchmarkSeries> Benchmark;
  std::optional<core::RuoniaSeries> Ruonia;
  std::int32_t PreviousDay = 0;
  if (!Algorithms.empty()) {
    const std::int32_t UpToYmd = Timeline->yyyymmddOf(Request.last_step());
    Ruonia = loadRuonia(Ctx, Market, UpToYmd);
    PreviousDay = previousTradingDay(Ctx, Market, *Timeline);
    const std::optional<std::int32_t> CompletedDay =
        core::lastCompletedDayYmd(*Timeline, Request.last_step());
    if (CompletedDay.has_value()) {
      Benchmark = loadBenchmark(Ctx, Market, *CompletedDay);
    }
  }

  const core::DivergenceResult Result =
      core::analyzeDivergences(Spec, Timeline, Journal, Request.last_step(),
                               Algorithms, Benchmark, Ruonia, PreviousDay);

  for (const core::Divergence &Div : Result.Divergences) {
    writeDivergence(Div, *Timeline, Out->add_divergences());
  }
  for (const core::MissedProfitableTrade &Item : Result.Missed) {
    writeMissed(Item, *Timeline, Out->add_missed_profitable());
  }
  for (const core::ConsensusItem &Item : Result.Consensus) {
    writeConsensus(Item, *Timeline, Out->add_consensus());
  }
  for (const core::AlgorithmPortfolio &Port : Result.Independent) {
    toProto(Port.Run, &Port.PortfolioReport, *Timeline, false,
            Out->add_algorithm_portfolios());
  }
}

} // namespace

// Ни одно исключение не выходит наружу (statusFromCurrentException). Ответ
// собирается отдельно и записывается в Response только при успехе.
grpc::Status handleAnalyzeDivergences(Context &Ctx,
                                      const pb::DivergenceRequest &Request,
                                      pb::DivergenceResponse *Response) {
  try {
    pb::DivergenceResponse Result;
    fill(Ctx, Request, &Result);
    *Response = std::move(Result);
    return grpc::Status::OK;
  } catch (...) {
    return statusFromCurrentException();
  }
}

} // namespace worker
