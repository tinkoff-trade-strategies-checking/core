#include "core/report/benchmark.hpp"

#include <algorithm>
#include <stdexcept>

#include "core/money/rounding.hpp"

namespace core {

std::optional<std::pair<Decimal, Decimal>>
benchmarkRange(const BenchmarkSeries &Series) {
  if (Series.Closes.empty())
    return std::nullopt;
  return std::make_pair(Series.FirstOpen, Series.Closes.back().Close);
}

std::vector<EquityPoint> benchmarkEquity(const BenchmarkSeries &Series,
                                         const Decimal &InitialCash,
                                         const Timeline &Timeline,
                                         std::int64_t LastStep) {
  if (Series.FirstOpen <= Decimal::zero())
    throw std::invalid_argument("benchmarkEquity: open первого дня <= 0");
  if (LastStep < 0 || LastStep >= Timeline.totalSteps()) {
    throw std::invalid_argument("benchmarkEquity: lastStep вне шкалы времени");
  }
  std::vector<EquityPoint> Out;
  for (std::int64_t Step = 0; Step <= LastStep; ++Step) {
    if (!Timeline.isLastStepOfDay(Step))
      continue;
    const std::int32_t Ymd = Timeline.yyyymmddOf(Step);
    const auto It =
        std::lower_bound(Series.Closes.begin(), Series.Closes.end(), Ymd,
                         [](const DailyClose &C, std::int32_t Value) {
                           return C.Yyyymmdd < Value;
                         });
    if (It == Series.Closes.end() || It->Yyyymmdd != Ymd)
      continue;
    Out.push_back(
        EquityPoint{Step, InitialCash.mulDiv(It->Close, Series.FirstOpen, 2,
                                             RoundingMode::HalfUp)});
  }
  return Out;
}

} // namespace core
