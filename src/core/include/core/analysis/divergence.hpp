#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/engine/journal.hpp"
#include "core/engine/session_engine.hpp"
#include "core/exec/order.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp"
#include "core/report/benchmark.hpp"
#include "core/report/metrics.hpp"
#include "core/report/ruonia.hpp"
#include "core/strategy/strategy_runner.hpp"

// Разбор расхождений между действиями человека и алгоритмов (ФТ-18, ФТ-19,
// ФТ-20, `decisions.md`, «Разбор расхождений», `dsl.md` v1). Режим «алгоритм
// на месте человека»: на каждом шаге стратегия видит бары только до этого шага
// и портфель человека на закрытии шага (INV-1). Цена ошибки считается тем же
// движком, что и сессия (INV-3). Без часов и случайности, обход только
// упорядоченных контейнеров (INV-4); деньги и доли от капитала — Decimal
// (INV-6).

namespace core {

enum class DivergenceType { Excess, Missed, Size, Timing };

// HumanStep / AlgoStep = -1, если действия на этой стороне нет (тогда лоты 0,
// OrderType — nullopt). Лоты со знаком: покупка +, продажа -.
// Cost = (AlgoValue - HumanValue) до копейки HALF_UP.
// CostPercent = (AlgoValue - HumanValue) * 100 / EquityAt, 2 знака HALF_UP
// (INV-6); 0, если EquityAt <= 0.
struct Divergence {
  std::string AlgorithmId;
  int Instrument = 0;
  DivergenceType Type = DivergenceType::Excess;
  std::int64_t HumanStep = -1;
  std::int64_t AlgoStep = -1;
  std::int64_t HumanLots = 0;
  std::int64_t AlgoLots = 0;
  std::int64_t HorizonStep = 0;
  Decimal Cost;
  Decimal CostPercent;
  std::string ReasonCode;
  std::int64_t Seq = 0;
  std::optional<OrderType> HumanOrderType;
  std::optional<OrderType> AlgoOrderType;
};

// Закрытая прибыльная сделка человека (покупка -> продажа по FIFO), вход
// которой алгоритм не сделал (ФТ-20).
struct MissedProfitableTrade {
  int Instrument = 0;
  std::int64_t EntryStep = 0;
  std::int64_t ExitStep = 0;
  std::int64_t Lots = 0;
  Decimal NetPnl;
  std::vector<std::string> AlgorithmIds;
  Decimal EntryPrice;
  Decimal ExitPrice;
  std::int64_t Seq = 0;
  std::vector<std::int64_t> DivergenceSeqs;
};

// Консенсус алгоритмов по действиям человека (при Agreeing < Total) и
// упущенным действиям. MedianCost — медиана Cost расхождений (при чётном
// числе — полусумма до копейки HALF_UP).
struct ConsensusItem {
  int Instrument = 0;
  std::int64_t Step = 0;
  Side OrderSide = Side::Buy;
  bool HumanAction = false;
  std::int32_t Agreeing = 0;
  std::int32_t Total = 0;
  Decimal MedianCost;
};

struct AlgorithmInput {
  std::string Id;
  std::string Source;
  std::map<std::string, double> ParamOverrides;
};

struct AlgorithmPortfolio {
  StrategyRunResult Run;
  Report PortfolioReport;
  std::uint64_t Digest = 0;
};

struct DivergenceResult {
  bool Cancelled = false;
  std::vector<Divergence> Divergences;
  std::vector<MissedProfitableTrade> Missed;
  std::vector<ConsensusItem> Consensus;
  std::vector<AlgorithmPortfolio> Independent;
};

// Разбирает расхождения человека и до 5 алгоритмов на шагах 0…LastStep
// (или до шага банкротства сессии человека, если он < LastStep).
// Нарушения входных условий и ошибки компиляции/E-LA* — std::invalid_argument.
DivergenceResult analyzeDivergences(
    const SessionSpec &Spec, std::shared_ptr<const Timeline> TimelineRef,
    std::span<const JournalEntry> Human, std::int64_t LastStep,
    const std::vector<AlgorithmInput> &Algorithms,
    const std::optional<BenchmarkSeries> &Benchmark = std::nullopt,
    const std::optional<RuoniaSeries> &Ruonia = std::nullopt,
    std::int32_t RuoniaPreviousDayYmd = 0,
    std::function<bool()> ShouldCancel = {});

} // namespace core
