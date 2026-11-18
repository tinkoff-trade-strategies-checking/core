#include "core/report/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "core/market/bar_source.hpp" // MarketDataError

// Все суммы double считаются обычными циклами в фиксированном порядке (INV-4):
// без параллелизма, без FMA
// (-ffp-contract=off) и без unordered-контейнеров.

namespace core {

namespace {

constexpr double kTradingDaysPerYear = 252.0;
// Разброс доходностей меньше этого порога считается нулевым: деление на него
// дало бы огромное или бесконечное число.
constexpr double kMinSpread = 1e-12;

// Число торговых дней в году для годовых метрик: sqrt(252).
double annualFactor() { return std::sqrt(kTradingDaysPerYear); }

struct Dd {
  double MaxDrawdown = 0;
  std::int64_t MaxSteps = 0;
};

// Просадка от пика; пик стартует с начального капитала. Шаги «ниже пика»
// считаются подряд, пока цена не вернётся на пик или выше.
Dd drawdown(const Decimal &InitialCash,
            const std::vector<EquityPoint> &Equity) {
  Dd Out;
  Decimal Peak = InitialCash;
  std::int64_t Run = 0;
  for (const EquityPoint &P : Equity) {
    if (P.Equity > Peak) {
      Peak = P.Equity;
      Run = 0;
    } else if (P.Equity < Peak) {
      Out.MaxDrawdown = std::max(Out.MaxDrawdown,
                                 1.0 - P.Equity.toDouble() / Peak.toDouble());
      Out.MaxSteps = std::max(Out.MaxSteps, ++Run);
    } else {
      Run = 0;
    }
  }
  return Out;
}

// Дневные доходности r_d = E_d / E_{d-1} - 1; E_d - стоимость на последнем шаге
// дня, E_0 - начальный капитал. Дни без бумажной стоимости (E_{d-1} <= 0) дают
// доходность 0: делить не на что.
struct Daily {
  std::vector<double> Returns;
  std::vector<double> Equities; // E_d в double
};

Daily dailySeries(const MetricsInput &In) {
  std::vector<Decimal> LastOfDay;
  std::int32_t CurrentDay = 0;
  for (std::size_t I = 0; I < In.Equity.size(); ++I) {
    const std::int32_t Day =
        In.TradingDayOfStep[static_cast<std::size_t>(In.Equity[I].Step)];
    if (I == 0 || Day != CurrentDay) {
      LastOfDay.push_back(In.Equity[I].Equity);
      CurrentDay = Day;
    } else {
      LastOfDay.back() = In.Equity[I].Equity;
    }
  }
  Daily Out;
  Decimal Prev = In.InitialCash;
  for (const Decimal &E : LastOfDay) {
    Out.Returns.push_back(
        Prev > Decimal::zero() ? E.toDouble() / Prev.toDouble() - 1.0 : 0.0);
    Out.Equities.push_back(E.toDouble());
    Prev = E;
  }
  return Out;
}

double mean(const std::vector<double> &V) {
  double Sum = 0;
  for (double X : V)
    Sum += X;
  return Sum / static_cast<double>(V.size());
}

// Выборочное стандартное отклонение (n - 1); v.size() >= 2.
double sampleStdev(const std::vector<double> &V, double M) {
  double Sum = 0;
  for (double X : V)
    Sum += (X - M) * (X - M);
  return std::sqrt(Sum / static_cast<double>(V.size() - 1));
}

void validate(const MetricsInput &In) {
  if (In.InitialCash <= Decimal::zero())
    throw std::invalid_argument("initialCash must be positive");
  std::int64_t PrevStep = -1;
  for (const EquityPoint &P : In.Equity) {
    if (P.Step <= PrevStep)
      throw std::invalid_argument("equity steps must be strictly increasing");
    if (P.Step >= static_cast<std::int64_t>(In.TradingDayOfStep.size()))
      throw std::invalid_argument("equity step is outside tradingDayOfStep");
    if (!In.PositionValuesByStep.empty() &&
        P.Step >= static_cast<std::int64_t>(In.PositionValuesByStep.size()))
      throw std::invalid_argument(
          "equity step is outside positionValuesByStep");
    PrevStep = P.Step;
  }
}

} // namespace

Report computeReport(const MetricsInput &In) {
  validate(In);

  Report Report;
  Report.Equity = In.Equity;
  Report.BenchmarkEquity = In.BenchmarkEquity;
  Report.RoundTrips = In.RoundTrips;
  Report.FinalEquity =
      In.Equity.empty() ? In.InitialCash : In.Equity.back().Equity;
  Metrics &M = Report.Stats;

  // Издержки: суммы в Decimal, в double только комиссия и сбор для валовой
  // доходности.
  Decimal TradedValue;
  for (const Fill &F : In.Fills) {
    Report.Costs.Commission += F.Commission;
    Report.Costs.Fee += F.Fee;
    Report.Costs.Spread += F.SpreadCost;
    Report.Costs.Slippage += F.SlippageCost;
    TradedValue += F.Value;
    if (F.StopAndTakeSameBar)
      ++Report.Stats.StopTakeSameBar;
  }

  const double E0 = In.InitialCash.toDouble();
  M.TotalReturn = Report.FinalEquity.toDouble() / E0 - 1.0;
  M.GrossReturn = M.TotalReturn +
                  (Report.Costs.Commission + Report.Costs.Fee).toDouble() / E0;

  const Daily Daily = dailySeries(In);
  M.TradingDays = static_cast<std::int32_t>(Daily.Returns.size());
  const double Days = static_cast<double>(M.TradingDays);
  if (M.TradingDays > 0) {
    // Без положительной итоговой стоимости степень не определена; без
    // положительной средней стоимости оборот 0: NaN и бесконечность в proto и
    // JSON не кодируются.
    if (Report.FinalEquity > Decimal::zero())
      M.Cagr = std::pow(Report.FinalEquity.toDouble() / E0,
                        kTradingDaysPerYear / Days) -
               1.0;
    const double MeanEquity = mean(Daily.Equities);
    if (MeanEquity > 0)
      M.Turnover = TradedValue.toDouble() / MeanEquity;
  }
  if (M.TradingDays >= 2) {
    if (In.RuoniaRates.size() != static_cast<std::size_t>(M.TradingDays))
      throw MarketDataError("нет ряда RUONIA для периода");
    const double MuR = mean(Daily.Returns);
    const double SdR = sampleStdev(Daily.Returns, MuR);
    M.Volatility = SdR * annualFactor();

    // Избыточные доходности e_d = r_d - r_{f,d}, конвенция Actual/365 по
    // календарным дням (core#26, decisions.md):
    // r_{f,d} = Rate / 100 * Delta_d / 365.
    std::vector<double> Excess;
    Excess.reserve(Daily.Returns.size());
    for (std::size_t I = 0; I < Daily.Returns.size(); ++I) {
      const double DeltaDays = (In.CalendarDays.size() == Daily.Returns.size())
                                   ? static_cast<double>(In.CalendarDays[I])
                                   : 1.0;
      const double Rf =
          (In.RuoniaRates[I].toDouble() / 100.0) * (DeltaDays / 365.0);
      Excess.push_back(Daily.Returns[I] - Rf);
    }

    const double MuE = mean(Excess);
    const double SdE = sampleStdev(Excess, MuE);
    if (SdE >= kMinSpread)
      M.Sharpe = MuE / SdE * annualFactor();
    double Downside = 0;
    for (double E : Excess) {
      const double Neg = std::min(E, 0.0);
      Downside += Neg * Neg;
    }
    const double DownsideDev = std::sqrt(Downside / Days);
    if (DownsideDev >= kMinSpread)
      M.Sortino = MuE / DownsideDev * annualFactor();
  }

  const Dd Dd = drawdown(In.InitialCash, In.Equity);
  M.MaxDrawdown = Dd.MaxDrawdown;
  M.MaxDrawdownSteps = Dd.MaxSteps;

  M.Fills = static_cast<std::int64_t>(In.Fills.size());
  M.RoundTrips = static_cast<std::int64_t>(In.RoundTrips.size());
  M.Rejections = In.Rejections;
  M.PartialFills = In.PartialFills;
  if (!In.RoundTrips.empty()) {
    std::int64_t Wins = 0;
    for (const RoundTrip &T : In.RoundTrips)
      if (T.NetPnl > Decimal::zero())
        ++Wins;
    M.WinRate =
        static_cast<double>(Wins) / static_cast<double>(In.RoundTrips.size());
  }

  // Концентрация: HHI - только по шагам с позициями, доля крупнейшей позиции -
  // от стоимости портфеля на шаге.
  if (!In.PositionValuesByStep.empty()) {
    double HhiSum = 0;
    std::int64_t HhiSteps = 0;
    for (const EquityPoint &P : In.Equity) {
      const std::vector<Decimal> &Values =
          In.PositionValuesByStep[static_cast<std::size_t>(P.Step)];
      Decimal Total, Largest;
      for (const Decimal &V : Values) {
        Total += V;
        if (V > Largest)
          Largest = V;
      }
      if (Total <= Decimal::zero())
        continue;
      const double TotalD = Total.toDouble();
      double Hhi = 0;
      for (const Decimal &V : Values) {
        const double W = V.toDouble() / TotalD;
        Hhi += W * W;
      }
      HhiSum += Hhi;
      ++HhiSteps;
      if (P.Equity > Decimal::zero())
        M.MaxPositionShare = std::max(M.MaxPositionShare,
                                      Largest.toDouble() / P.Equity.toDouble());
    }
    if (HhiSteps > 0)
      M.HhiAvg = HhiSum / static_cast<double>(HhiSteps);
  }

  if (In.Benchmark && In.Benchmark->first > Decimal::zero())
    M.BenchmarkReturn =
        In.Benchmark->second.toDouble() / In.Benchmark->first.toDouble() - 1.0;

  return Report;
}

} // namespace core
