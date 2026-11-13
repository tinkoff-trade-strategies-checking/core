#include "core/report/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

// Все суммы double считаются обычными циклами в фиксированном порядке (INV-4): без параллелизма, без FMA
// (-ffp-contract=off) и без unordered-контейнеров.

namespace core {

namespace {

constexpr double kTradingDaysPerYear = 252.0;
// Разброс доходностей меньше этого порога считается нулевым: деление на него дало бы огромное или бесконечное число.
constexpr double kMinSpread = 1e-12;

// Число торговых дней в году для годовых метрик: sqrt(252).
double annualFactor() { return std::sqrt(kTradingDaysPerYear); }

struct Dd {
  double maxDrawdown = 0;
  std::int64_t maxSteps = 0;
};

// Просадка от пика; пик стартует с начального капитала. Шаги «ниже пика» считаются подряд, пока цена не вернётся
// на пик или выше.
Dd drawdown(const Decimal& initialCash, const std::vector<EquityPoint>& equity) {
  Dd out;
  Decimal peak = initialCash;
  std::int64_t run = 0;
  for (const EquityPoint& p : equity) {
    if (p.equity > peak) {
      peak = p.equity;
      run = 0;
    } else if (p.equity < peak) {
      out.maxDrawdown = std::max(out.maxDrawdown, 1.0 - p.equity.toDouble() / peak.toDouble());
      out.maxSteps = std::max(out.maxSteps, ++run);
    } else {
      run = 0;
    }
  }
  return out;
}

// Дневные доходности r_d = E_d / E_{d-1} - 1; E_d - стоимость на последнем шаге дня, E_0 - начальный капитал.
// Дни без бумажной стоимости (E_{d-1} <= 0) дают доходность 0: делить не на что.
struct Daily {
  std::vector<double> returns;
  std::vector<double> equities;  // E_d в double
};

Daily dailySeries(const MetricsInput& in) {
  std::vector<Decimal> lastOfDay;
  std::int32_t currentDay = 0;
  for (std::size_t i = 0; i < in.equity.size(); ++i) {
    const std::int32_t day = in.tradingDayOfStep[static_cast<std::size_t>(in.equity[i].step)];
    if (i == 0 || day != currentDay) {
      lastOfDay.push_back(in.equity[i].equity);
      currentDay = day;
    } else {
      lastOfDay.back() = in.equity[i].equity;
    }
  }
  Daily out;
  Decimal prev = in.initialCash;
  for (const Decimal& e : lastOfDay) {
    out.returns.push_back(prev > Decimal::zero() ? e.toDouble() / prev.toDouble() - 1.0 : 0.0);
    out.equities.push_back(e.toDouble());
    prev = e;
  }
  return out;
}

double mean(const std::vector<double>& v) {
  double sum = 0;
  for (double x : v) sum += x;
  return sum / static_cast<double>(v.size());
}

// Выборочное стандартное отклонение (n - 1); v.size() >= 2.
double sampleStdev(const std::vector<double>& v, double m) {
  double sum = 0;
  for (double x : v) sum += (x - m) * (x - m);
  return std::sqrt(sum / static_cast<double>(v.size() - 1));
}

void validate(const MetricsInput& in) {
  if (in.initialCash <= Decimal::zero()) throw std::invalid_argument("initialCash must be positive");
  std::int64_t prevStep = -1;
  for (const EquityPoint& p : in.equity) {
    if (p.step <= prevStep) throw std::invalid_argument("equity steps must be strictly increasing");
    if (p.step >= static_cast<std::int64_t>(in.tradingDayOfStep.size()))
      throw std::invalid_argument("equity step is outside tradingDayOfStep");
    if (!in.positionValuesByStep.empty() && p.step >= static_cast<std::int64_t>(in.positionValuesByStep.size()))
      throw std::invalid_argument("equity step is outside positionValuesByStep");
    prevStep = p.step;
  }
}

}  // namespace

Report computeReport(const MetricsInput& in) {
  validate(in);

  Report report;
  report.equity = in.equity;
  report.benchmarkEquity = in.benchmarkEquity;
  report.finalEquity = in.equity.empty() ? in.initialCash : in.equity.back().equity;
  Metrics& m = report.metrics;

  // Издержки: суммы в Decimal, в double только комиссия и сбор для валовой доходности.
  Decimal tradedValue;
  for (const Fill& f : in.fills) {
    report.costs.commission += f.commission;
    report.costs.fee += f.fee;
    report.costs.spread += f.spreadCost;
    report.costs.slippage += f.slippageCost;
    tradedValue += f.value;
    if (f.stopAndTakeSameBar) ++report.metrics.stopTakeSameBar;
  }

  const double e0 = in.initialCash.toDouble();
  m.totalReturn = report.finalEquity.toDouble() / e0 - 1.0;
  m.grossReturn = m.totalReturn + (report.costs.commission + report.costs.fee).toDouble() / e0;

  const Daily daily = dailySeries(in);
  m.tradingDays = static_cast<std::int32_t>(daily.returns.size());
  const double days = static_cast<double>(m.tradingDays);
  if (m.tradingDays > 0) {
    // Без положительной итоговой стоимости степень не определена; без положительной средней стоимости оборот 0:
    // NaN и бесконечность в proto и JSON не кодируются.
    if (report.finalEquity > Decimal::zero())
      m.cagr = std::pow(report.finalEquity.toDouble() / e0, kTradingDaysPerYear / days) - 1.0;
    const double meanEquity = mean(daily.equities);
    if (meanEquity > 0) m.turnover = tradedValue.toDouble() / meanEquity;
  }
  if (m.tradingDays >= 2) {
    const double mu = mean(daily.returns);
    const double sd = sampleStdev(daily.returns, mu);
    m.volatility = sd * annualFactor();
    if (sd >= kMinSpread) m.sharpe = mu / sd * annualFactor();
    double downside = 0;
    for (double r : daily.returns) {
      const double neg = std::min(r, 0.0);
      downside += neg * neg;
    }
    const double downsideDev = std::sqrt(downside / days);
    if (downsideDev >= kMinSpread) m.sortino = mu / downsideDev * annualFactor();
  }

  const Dd dd = drawdown(in.initialCash, in.equity);
  m.maxDrawdown = dd.maxDrawdown;
  m.maxDrawdownSteps = dd.maxSteps;

  m.fills = static_cast<std::int64_t>(in.fills.size());
  m.roundTrips = static_cast<std::int64_t>(in.roundTrips.size());
  m.rejections = in.rejections;
  m.partialFills = in.partialFills;
  if (!in.roundTrips.empty()) {
    std::int64_t wins = 0;
    for (const RoundTrip& t : in.roundTrips)
      if (t.netPnl > Decimal::zero()) ++wins;
    m.winRate = static_cast<double>(wins) / static_cast<double>(in.roundTrips.size());
  }

  // Концентрация: HHI - только по шагам с позициями, доля крупнейшей позиции - от стоимости портфеля на шаге.
  if (!in.positionValuesByStep.empty()) {
    double hhiSum = 0;
    std::int64_t hhiSteps = 0;
    for (const EquityPoint& p : in.equity) {
      const std::vector<Decimal>& values = in.positionValuesByStep[static_cast<std::size_t>(p.step)];
      Decimal total, largest;
      for (const Decimal& v : values) {
        total += v;
        if (v > largest) largest = v;
      }
      if (total <= Decimal::zero()) continue;
      const double totalD = total.toDouble();
      double hhi = 0;
      for (const Decimal& v : values) {
        const double w = v.toDouble() / totalD;
        hhi += w * w;
      }
      hhiSum += hhi;
      ++hhiSteps;
      if (p.equity > Decimal::zero())
        m.maxPositionShare = std::max(m.maxPositionShare, largest.toDouble() / p.equity.toDouble());
    }
    if (hhiSteps > 0) m.hhiAvg = hhiSum / static_cast<double>(hhiSteps);
  }

  if (in.benchmark && in.benchmark->first > Decimal::zero())
    m.benchmarkReturn = in.benchmark->second.toDouble() / in.benchmark->first.toDouble() - 1.0;

  return report;
}

}  // namespace core
