#include "core/analysis/walk_forward.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace core {

namespace {

// Допуск на накопленную ошибку double при подсчёте числа шагов диапазона ((до -
// от) / шаг).
constexpr double kStepEpsilon = 1e-9;

// Число значений параметра в сетке: без диапазона - одно (по умолчанию); иначе
// от, от + шаг, … <= до.
std::int64_t valueCount(const dsl::ResolvedParam &P) {
  if (!P.Min || !P.Max || !P.Step)
    return 1;
  if (!(*P.Step > 0) || !(*P.Max >= *P.Min)) {
    throw std::invalid_argument("параметр `" + P.Name + "`: неверный диапазон");
  }
  const double Count =
      std::floor((*P.Max - *P.Min) / *P.Step + kStepEpsilon) + 1;
  // Заведомо больше любой сетки (и безопасно для int64); точный предел
  // проверяет paramGrid.
  if (Count > 1e15)
    return static_cast<std::int64_t>(1e15);
  return static_cast<std::int64_t>(Count);
}

// Значение от + i * шаг: умножением, а не накоплением суммы (ошибка не растёт);
// не больше «до».
double valueAt(const dsl::ResolvedParam &P, std::int64_t I) {
  if (!P.Min || !P.Max || !P.Step)
    return P.Value;
  return std::min(*P.Min + static_cast<double>(I) * *P.Step, *P.Max);
}

// Вход метрик по результату прогона на срезе шкалы (то же, что делает воркер
// для RunOutput; бенчмарка нет).
MetricsInput metricsInputOf(const RunOutput &Output, const Decimal &InitialCash,
                            const Timeline &Timeline) {
  MetricsInput In;
  In.InitialCash = InitialCash;
  In.Equity = Output.Equity;
  In.TradingDayOfStep.reserve(static_cast<std::size_t>(Output.LastStep) + 1);
  for (std::int64_t Step = 0; Step <= Output.LastStep; ++Step)
    In.TradingDayOfStep.push_back(Timeline.tradingDayOf(Step));
  In.Fills = Output.Fills;
  In.RoundTrips = Output.RoundTrips;
  In.Rejections = Output.Rejections;
  In.PartialFills = Output.PartialFills;
  In.PositionValuesByStep = Output.PositionValuesByStep;
  return In;
}

double netReturnOf(const RunOutput &Output, const Decimal &InitialCash) {
  return (Output.FinalEquity - InitialCash).toDouble() / InitialCash.toDouble();
}

struct Window {
  std::int32_t FitFrom, FitTo, TestFrom, TestTo;
};

std::vector<Window> windowsOf(std::int32_t TradingDays,
                              const WalkForwardConfig &C) {
  std::vector<Window> Windows;
  for (std::int64_t K = 0;; ++K) {
    const std::int64_t Shift = K * C.ShiftDays;
    const std::int64_t TestTo = Shift + C.FitDays + C.TestDays;
    if (TestTo > TradingDays)
      break;
    Windows.push_back(Window{static_cast<std::int32_t>(1 + Shift),
                             static_cast<std::int32_t>(Shift + C.FitDays),
                             static_cast<std::int32_t>(Shift + C.FitDays + 1),
                             static_cast<std::int32_t>(TestTo)});
  }
  return Windows;
}

// Накопитель прогресса: общий счётчик шагов по всем прогонам. После прогона
// сдвиг растёт на длину среза, поэтому done не убывает и при досрочной
// остановке прогона (банкротство).
class ProgressMeter {
public:
  ProgressMeter(const Progress &OnProgress, std::int64_t TotalSteps)
      : Callback(OnProgress), Total(TotalSteps) {}

  Progress forRun() {
    return [this](std::int64_t Done, std::int64_t) { report(Offset + Done); };
  }
  void finishRun(std::int64_t SliceSteps) {
    Offset += SliceSteps;
    report(Offset);
  }

private:
  void report(std::int64_t Done) {
    if (!Callback || Done == Reported)
      return;
    Reported = Done;
    Callback(Done, Total);
  }

  const Progress &Callback;
  std::int64_t Total;
  std::int64_t Offset = 0, Reported = -1;
};

} // namespace

std::vector<dsl::ParamValues>
paramGrid(const std::vector<dsl::ResolvedParam> &Params, std::int32_t MaxGrid) {
  if (MaxGrid < 1)
    throw std::invalid_argument(
        "walk-forward: максимальный размер сетки должен быть не меньше 1");
  std::vector<std::int64_t> Counts;
  std::int64_t Size = 1;
  for (const dsl::ResolvedParam &P : Params) {
    const std::int64_t N = valueCount(P);
    Counts.push_back(N);
    // Проверка до умножения: ни переполнения, ни лишней памяти.
    if (N > MaxGrid || Size > MaxGrid / N) {
      throw std::invalid_argument("walk-forward: сетка параметров больше " +
                                  std::to_string(MaxGrid) +
                                  " комбинаций; увеличьте шаг параметров");
    }
    Size *= N;
  }

  std::vector<dsl::ParamValues> Grid;
  Grid.reserve(static_cast<std::size_t>(Size));
  std::vector<std::int64_t> Index(Params.size(), 0);
  for (std::int64_t Combination = 0; Combination < Size; ++Combination) {
    dsl::ParamValues Values;
    for (std::size_t I = 0; I < Params.size(); ++I)
      Values[Params[I].Name] = valueAt(Params[I], Index[I]);
    Grid.push_back(std::move(Values));
    // Счётчик с переносом: последний параметр меняется быстрее всех.
    for (std::size_t I = Params.size(); I-- > 0;) {
      if (++Index[I] < Counts[I])
        break;
      Index[I] = 0;
    }
  }
  return Grid;
}

double chainedReturn(const std::vector<double> &Returns) {
  double Product = 1.0;
  for (const double R : Returns)
    Product *= 1.0 + R;
  return Product - 1.0;
}

WalkForwardResult walkForward(const SessionSpec &Spec,
                              std::shared_ptr<const Timeline> Tl,
                              const dsl::Program &Program,
                              const std::vector<dsl::ResolvedParam> &Params,
                              const WalkForwardConfig &Config,
                              const Progress &OnProgress) {
  if (Config.FitDays < 1 || Config.TestDays < 1 || Config.ShiftDays < 1) {
    throw std::invalid_argument("walk-forward: окна подбора, проверки и сдвиг "
                                "должны быть не меньше 1 дня");
  }
  const std::vector<dsl::ParamValues> Grid = paramGrid(Params, Config.MaxGrid);
  const std::vector<Window> Windows = windowsOf(Tl->tradingDays(), Config);
  if (Windows.empty()) {
    throw std::invalid_argument(
        "walk-forward: период короче одного отрезка (подбор " +
        std::to_string(Config.FitDays) + " + проверка " +
        std::to_string(Config.TestDays) + " торговых дней, в шкале " +
        std::to_string(Tl->tradingDays()) + ")");
  }

  // Шагов в каждом торговом дне (дни с 1), накопленно: по ним считается общий
  // объём работы для прогресса.
  std::vector<std::int64_t> Prefix(
      static_cast<std::size_t>(Tl->tradingDays()) + 1, 0);
  for (std::int64_t Step = 0; Step < Tl->totalSteps(); ++Step) {
    ++Prefix[static_cast<std::size_t>(Tl->tradingDayOf(Step))];
  }
  for (std::size_t Day = 1; Day < Prefix.size(); ++Day)
    Prefix[Day] += Prefix[Day - 1];
  const auto StepsIn = [&](std::int32_t From, std::int32_t To) {
    return Prefix[static_cast<std::size_t>(To)] -
           Prefix[static_cast<std::size_t>(From) - 1];
  };
  std::int64_t Total = 0;
  for (const Window &W : Windows) {
    Total +=
        static_cast<std::int64_t>(Grid.size()) * StepsIn(W.FitFrom, W.FitTo) +
        StepsIn(W.TestFrom, W.TestTo);
  }
  ProgressMeter Meter(OnProgress, Total);

  WalkForwardResult Result;
  std::vector<double> TestReturns;
  for (std::size_t K = 0; K < Windows.size(); ++K) {
    const Window &W = Windows[K];
    const auto FitTimeline =
        std::make_shared<const Timeline>(Tl->slice(W.FitFrom, W.FitTo));
    const std::int64_t FitSteps = FitTimeline->totalSteps();

    // Строго «больше»: при равенстве остаётся более ранний, то есть
    // лексикографически меньший набор.
    const dsl::ParamValues *Best = nullptr;
    double BestReturn = 0;
    for (const dsl::ParamValues &Values : Grid) {
      const RunOutput Fit = runBacktest(Spec, FitTimeline, Program, Params,
                                        Values, dsl::Level::Ok, Meter.forRun());
      Meter.finishRun(FitSteps);
      if (Fit.Status == RunStatus::Invalid)
        continue; // E-RT01: прогон не засчитывается
      const double R = netReturnOf(Fit, Spec.InitialCash);
      if (Best == nullptr || R > BestReturn) {
        Best = &Values;
        BestReturn = R;
      }
    }
    if (Best == nullptr) {
      throw NoValidFitRun("walk-forward: все прогоны подбора остановлены "
                          "нарушением целостности (E-RT01)");
    }

    const auto TestTimeline =
        std::make_shared<const Timeline>(Tl->slice(W.TestFrom, W.TestTo));
    WalkForwardSegment Segment;
    Segment.Index = static_cast<std::int32_t>(K);
    Segment.FitFromDay = W.FitFrom;
    Segment.FitToDay = W.FitTo;
    Segment.TestFromDay = W.TestFrom;
    Segment.TestToDay = W.TestTo;
    Segment.Chosen = *Best;
    Segment.FitNetReturn = BestReturn;
    Segment.Test = runBacktest(Spec, TestTimeline, Program, Params, *Best,
                               dsl::Level::Ok, Meter.forRun());
    Meter.finishRun(TestTimeline->totalSteps());
    Segment.TestReport = computeReport(
        metricsInputOf(Segment.Test, Spec.InitialCash, *TestTimeline));
    TestReturns.push_back(netReturnOf(Segment.Test, Spec.InitialCash));
    Result.Segments.push_back(std::move(Segment));
  }
  Result.ChainedReturn = chainedReturn(TestReturns);
  return Result;
}

} // namespace core
