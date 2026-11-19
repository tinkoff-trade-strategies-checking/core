#include "core/report/metrics_input.hpp"

#include <cstddef>
#include <utility>
#include <vector>

namespace core {

namespace {

std::vector<std::int32_t> tradingDays(const Timeline &Timeline,
                                      std::int64_t LastStep) {
  std::vector<std::int32_t> Days;
  Days.reserve(static_cast<std::size_t>(LastStep) + 1);
  for (std::int64_t Step = 0; Step <= LastStep; ++Step)
    Days.push_back(Timeline.tradingDayOf(Step));
  return Days;
}

void countOrders(const std::vector<Order> &Orders, MetricsInput *In) {
  for (const Order &O : Orders) {
    if (O.Status == OrderStatus::Rejected)
      ++In->Rejections;
    if (O.FilledLots > 0 && O.FilledLots < O.Spec.Lots)
      ++In->PartialFills;
  }
}

void setBenchmark(const std::optional<BenchmarkSeries> &Benchmark,
                  const Timeline &Timeline, MetricsInput *In) {
  if (!Benchmark)
    return;
  In->Benchmark = benchmarkRange(*Benchmark);
  // open первого дня <= 0 — испорченный бар индекса: доходности нет
  // (computeReport), кривой тоже, отчёт считается.
  if (In->Benchmark && In->Benchmark->first > Decimal::zero() &&
      !In->Equity.empty()) {
    In->BenchmarkEquity = benchmarkEquity(*Benchmark, In->InitialCash, Timeline,
                                          In->Equity.back().Step);
  }
}

} // namespace

std::optional<std::int32_t> lastCompletedDayYmd(const Timeline &Timeline,
                                                std::int64_t LastStep) {
  std::int64_t Step = LastStep;
  if (!Timeline.isLastStepOfDay(Step)) {
    const std::int32_t Today = Timeline.yyyymmddOf(Step);
    while (Step >= 0 && Timeline.yyyymmddOf(Step) == Today)
      --Step;
    if (Step < 0)
      return std::nullopt;
  }
  return Timeline.yyyymmddOf(Step);
}

MetricsInput buildMetricsInput(const SessionEngine &Engine,
                               const Decimal &InitialCash,
                               const Timeline &Timeline,
                               const std::optional<BenchmarkSeries> &Benchmark,
                               const std::optional<RuoniaSeries> &Ruonia,
                               std::int32_t RuoniaPreviousDayYmd) {
  MetricsInput In;
  In.InitialCash = InitialCash;
  In.Equity = Engine.equity();
  In.TradingDayOfStep = tradingDays(Timeline, Engine.currentStep());
  In.Fills = Engine.fills();
  In.RoundTrips = Engine.portfolio().roundTrips();
  countOrders(Engine.orders(), &In);
  In.PositionValuesByStep = Engine.positionValues();
  setBenchmark(Benchmark, Timeline, &In);
  if (Ruonia) {
    MatchedRuonia Matched = matchRuoniaRates(
        *Ruonia, Timeline, Engine.currentStep(), RuoniaPreviousDayYmd);
    In.RuoniaRates = std::move(Matched.Rates);
    In.CalendarDays = std::move(Matched.CalendarDays);
  }
  return In;
}

} // namespace core
