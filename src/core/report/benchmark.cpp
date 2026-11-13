#include "core/report/benchmark.hpp"

#include <algorithm>
#include <stdexcept>

#include "core/money/rounding.hpp"

namespace core {

std::optional<std::pair<Decimal, Decimal>> benchmarkRange(const BenchmarkSeries& series) {
  if (series.closes.empty()) return std::nullopt;
  return std::make_pair(series.firstOpen, series.closes.back().close);
}

std::vector<EquityPoint> benchmarkEquity(const BenchmarkSeries& series, const Decimal& initialCash,
                                         const Timeline& timeline, std::int64_t lastStep) {
  if (series.firstOpen <= Decimal::zero()) throw std::invalid_argument("benchmarkEquity: open первого дня <= 0");
  if (lastStep < 0 || lastStep >= timeline.totalSteps()) {
    throw std::invalid_argument("benchmarkEquity: lastStep вне шкалы времени");
  }
  std::vector<EquityPoint> out;
  for (std::int64_t step = 0; step <= lastStep; ++step) {
    if (!timeline.isLastStepOfDay(step)) continue;
    const std::int32_t ymd = timeline.yyyymmddOf(step);
    const auto it = std::lower_bound(series.closes.begin(), series.closes.end(), ymd,
                                     [](const DailyClose& c, std::int32_t value) { return c.yyyymmdd < value; });
    if (it == series.closes.end() || it->yyyymmdd != ymd) continue;
    out.push_back(EquityPoint{step, initialCash.mul(it->close, RoundingMode::HALF_UP)
                                        .div(series.firstOpen, RoundingMode::HALF_UP)});
  }
  return out;
}

}  // namespace core
