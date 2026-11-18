#include "core/analysis/period_classifier.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace core {

namespace {

constexpr double kTradingDaysPerYear = 252.0;
constexpr double kCalmMaxMdd = 0.10;
constexpr double kStressMinMdd = 0.20;

// Линейная интерполяция перцентиля (метод Type 7: pos = q * (N - 1),
// значение = sorted[lo] + (pos - lo) * (sorted[hi] - sorted[lo])).
// Требует непустого отсортированного по возрастанию вектора.
double percentileLinear(const std::vector<double> &Sorted, double Q) {
  if (Sorted.size() == 1)
    return Sorted[0];
  const double Pos = Q * static_cast<double>(Sorted.size() - 1);
  const auto Lo = static_cast<std::size_t>(std::floor(Pos));
  const auto Hi = static_cast<std::size_t>(std::ceil(Pos));
  const double Frac = Pos - static_cast<double>(Lo);
  return Sorted[Lo] + Frac * (Sorted[Hi] - Sorted[Lo]);
}

PeriodCategory categoryOf(double Sigma, double Mdd, double P33, double P80) {
  // Stress проверяется раньше Calm: при совпадении условий (например, P33 ==
  // P80) побеждает более консервативный стрессовый режим.
  if (Sigma >= P80 || Mdd >= kStressMinMdd)
    return PeriodCategory::Stress;
  if (Sigma <= P33 && Mdd < kCalmMaxMdd)
    return PeriodCategory::Calm;
  return PeriodCategory::Medium;
}

void validateSeries(std::span<const DailyClose> Series,
                    std::int32_t WindowLength, std::vector<double> &ClosesOut) {
  if (WindowLength < 3) {
    throw std::invalid_argument(
        "длина окна должна быть не меньше 3 торговых дней");
  }
  ClosesOut.reserve(Series.size());
  for (std::size_t I = 0; I < Series.size(); ++I) {
    if (I > 0 && Series[I].Yyyymmdd <= Series[I - 1].Yyyymmdd) {
      throw std::invalid_argument(
          "даты дневного ряда индекса должны строго возрастать");
    }
    const double C = Series[I].Close.toDouble();
    if (!(C > 0.0)) {
      throw std::invalid_argument(
          "цена закрытия индекса должна быть положительной");
    }
    ClosesOut.push_back(C);
  }
}

} // namespace

PeriodClassification classifyPeriods(std::span<const DailyClose> Series,
                                     std::int32_t WindowLength) {
  std::vector<double> Closes;
  validateSeries(Series, WindowLength, Closes);

  PeriodClassification Out;
  const auto Len = static_cast<std::size_t>(WindowLength);
  if (Series.size() < Len)
    return Out;

  const std::size_t WindowCount = Series.size() - Len + 1;
  Out.Windows.reserve(WindowCount);
  std::vector<double> Sigmas;
  Sigmas.reserve(WindowCount);

  std::vector<double> LogReturns(Series.size() - 1);
  for (std::size_t I = 1; I < Series.size(); ++I)
    LogReturns[I - 1] = std::log(Closes[I] / Closes[I - 1]);

  const std::size_t ReturnCount = Len - 1;
  const double InvReturnCount = 1.0 / static_cast<double>(ReturnCount);
  const double InvSampleDenom = 1.0 / static_cast<double>(ReturnCount - 1);
  const double AnnualFactor = std::sqrt(kTradingDaysPerYear);

  for (std::size_t Start = 0; Start < WindowCount; ++Start) {
    double Sum = 0.0;
    for (std::size_t K = 0; K < ReturnCount; ++K)
      Sum += LogReturns[Start + K];
    const double Mean = Sum * InvReturnCount;

    double SqSum = 0.0;
    for (std::size_t K = 0; K < ReturnCount; ++K) {
      const double Diff = LogReturns[Start + K] - Mean;
      SqSum += Diff * Diff;
    }
    const double Sigma = std::sqrt(SqSum * InvSampleDenom) * AnnualFactor;

    double Peak = Closes[Start];
    double Mdd = 0.0;
    for (std::size_t K = 1; K < Len; ++K) {
      const double Price = Closes[Start + K];
      if (Price > Peak)
        Peak = Price;
      const double Drawdown = 1.0 - Price / Peak;
      if (Drawdown > Mdd)
        Mdd = Drawdown;
    }

    PeriodWindow W;
    W.StartDate = Series[Start].Yyyymmdd;
    W.EndDate = Series[Start + Len - 1].Yyyymmdd;
    W.Sigma = Sigma;
    W.Mdd = Mdd;
    Out.Windows.push_back(W);
    Sigmas.push_back(Sigma);
  }

  std::sort(Sigmas.begin(), Sigmas.end());
  Out.P33 = percentileLinear(Sigmas, 0.33);
  Out.P80 = percentileLinear(Sigmas, 0.80);

  for (PeriodWindow &W : Out.Windows)
    W.Category = categoryOf(W.Sigma, W.Mdd, Out.P33, Out.P80);

  return Out;
}

PeriodClassification classifyPeriodAt(std::span<const DailyClose> Series,
                                      std::int32_t WindowLength,
                                      std::int32_t StartDate) {
  PeriodClassification All = classifyPeriods(Series, WindowLength);
  for (const PeriodWindow &W : All.Windows) {
    if (W.StartDate == StartDate) {
      PeriodClassification Single;
      Single.P33 = All.P33;
      Single.P80 = All.P80;
      Single.Windows.push_back(W);
      return Single;
    }
  }
  throw std::invalid_argument(
      "окно с указанной датой начала не найдено в периоде снапшота");
}

} // namespace core
