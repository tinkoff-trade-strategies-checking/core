#include "core/analysis/walk_forward.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace core {

namespace {

// Допуск на накопленную ошибку double при подсчёте числа шагов диапазона ((до - от) / шаг).
constexpr double kStepEpsilon = 1e-9;

// Число значений параметра в сетке: без диапазона - одно (по умолчанию); иначе от, от + шаг, … <= до.
std::int64_t valueCount(const dsl::ResolvedParam& p) {
  if (!p.min || !p.max || !p.step) return 1;
  if (!(*p.step > 0) || !(*p.max >= *p.min)) {
    throw std::invalid_argument("параметр `" + p.name + "`: неверный диапазон");
  }
  const double count = std::floor((*p.max - *p.min) / *p.step + kStepEpsilon) + 1;
  // Заведомо больше любой сетки (и безопасно для int64); точный предел проверяет paramGrid.
  if (count > 1e15) return static_cast<std::int64_t>(1e15);
  return static_cast<std::int64_t>(count);
}

// Значение от + i * шаг: умножением, а не накоплением суммы (ошибка не растёт); не больше «до».
double valueAt(const dsl::ResolvedParam& p, std::int64_t i) {
  if (!p.min || !p.max || !p.step) return p.value;
  return std::min(*p.min + static_cast<double>(i) * *p.step, *p.max);
}

// Вход метрик по результату прогона на срезе шкалы (то же, что делает воркер для RunOutput; бенчмарка нет).
MetricsInput metricsInputOf(const RunOutput& output, const Decimal& initialCash, const Timeline& timeline) {
  MetricsInput in;
  in.initialCash = initialCash;
  in.equity = output.equity;
  in.tradingDayOfStep.reserve(static_cast<std::size_t>(output.lastStep) + 1);
  for (std::int64_t step = 0; step <= output.lastStep; ++step) in.tradingDayOfStep.push_back(timeline.tradingDayOf(step));
  in.fills = output.fills;
  in.roundTrips = output.roundTrips;
  in.rejections = output.rejections;
  in.partialFills = output.partialFills;
  in.positionValuesByStep = output.positionValuesByStep;
  return in;
}

double netReturnOf(const RunOutput& output, const Decimal& initialCash) {
  return (output.finalEquity - initialCash).toDouble() / initialCash.toDouble();
}

struct Window {
  std::int32_t fitFrom, fitTo, testFrom, testTo;
};

std::vector<Window> windowsOf(std::int32_t tradingDays, const WalkForwardConfig& c) {
  std::vector<Window> windows;
  for (std::int64_t k = 0;; ++k) {
    const std::int64_t shift = k * c.shiftDays;
    const std::int64_t testTo = shift + c.fitDays + c.testDays;
    if (testTo > tradingDays) break;
    windows.push_back(Window{static_cast<std::int32_t>(1 + shift), static_cast<std::int32_t>(shift + c.fitDays),
                             static_cast<std::int32_t>(shift + c.fitDays + 1), static_cast<std::int32_t>(testTo)});
  }
  return windows;
}

// Накопитель прогресса: общий счётчик шагов по всем прогонам. После прогона сдвиг растёт на длину среза, поэтому
// done не убывает и при досрочной остановке прогона (банкротство).
class ProgressMeter {
 public:
  ProgressMeter(const Progress& progress, std::int64_t total) : progress_(progress), total_(total) {}

  Progress forRun() {
    return [this](std::int64_t done, std::int64_t) { report(offset_ + done); };
  }
  void finishRun(std::int64_t sliceSteps) {
    offset_ += sliceSteps;
    report(offset_);
  }

 private:
  void report(std::int64_t done) {
    if (!progress_ || done == reported_) return;
    reported_ = done;
    progress_(done, total_);
  }

  const Progress& progress_;
  std::int64_t total_;
  std::int64_t offset_ = 0, reported_ = -1;
};

}  // namespace

std::vector<dsl::ParamValues> paramGrid(const std::vector<dsl::ResolvedParam>& params, std::int32_t maxGrid) {
  if (maxGrid < 1) throw std::invalid_argument("walk-forward: максимальный размер сетки должен быть не меньше 1");
  std::vector<std::int64_t> counts;
  std::int64_t size = 1;
  for (const dsl::ResolvedParam& p : params) {
    const std::int64_t n = valueCount(p);
    counts.push_back(n);
    // Проверка до умножения: ни переполнения, ни лишней памяти.
    if (n > maxGrid || size > maxGrid / n) {
      throw std::invalid_argument("walk-forward: сетка параметров больше " + std::to_string(maxGrid) +
                                  " комбинаций; увеличьте шаг параметров");
    }
    size *= n;
  }

  std::vector<dsl::ParamValues> grid;
  grid.reserve(static_cast<std::size_t>(size));
  std::vector<std::int64_t> index(params.size(), 0);
  for (std::int64_t combination = 0; combination < size; ++combination) {
    dsl::ParamValues values;
    for (std::size_t i = 0; i < params.size(); ++i) values[params[i].name] = valueAt(params[i], index[i]);
    grid.push_back(std::move(values));
    // Счётчик с переносом: последний параметр меняется быстрее всех.
    for (std::size_t i = params.size(); i-- > 0;) {
      if (++index[i] < counts[i]) break;
      index[i] = 0;
    }
  }
  return grid;
}

double chainedReturn(const std::vector<double>& returns) {
  double product = 1.0;
  for (const double r : returns) product *= 1.0 + r;
  return product - 1.0;
}

WalkForwardResult walkForward(const SessionSpec& spec, std::shared_ptr<const Timeline> timeline,
                              const dsl::Program& program, const std::vector<dsl::ResolvedParam>& params,
                              const WalkForwardConfig& config, const Progress& progress) {
  if (config.fitDays < 1 || config.testDays < 1 || config.shiftDays < 1) {
    throw std::invalid_argument("walk-forward: окна подбора, проверки и сдвиг должны быть не меньше 1 дня");
  }
  const std::vector<dsl::ParamValues> grid = paramGrid(params, config.maxGrid);
  const std::vector<Window> windows = windowsOf(timeline->tradingDays(), config);
  if (windows.empty()) {
    throw std::invalid_argument("walk-forward: период короче одного отрезка (подбор " + std::to_string(config.fitDays) +
                                " + проверка " + std::to_string(config.testDays) + " торговых дней, в шкале " +
                                std::to_string(timeline->tradingDays()) + ")");
  }

  // Шагов в каждом торговом дне (дни с 1), накопленно: по ним считается общий объём работы для прогресса.
  std::vector<std::int64_t> prefix(static_cast<std::size_t>(timeline->tradingDays()) + 1, 0);
  for (std::int64_t step = 0; step < timeline->totalSteps(); ++step) {
    ++prefix[static_cast<std::size_t>(timeline->tradingDayOf(step))];
  }
  for (std::size_t day = 1; day < prefix.size(); ++day) prefix[day] += prefix[day - 1];
  const auto stepsIn = [&](std::int32_t from, std::int32_t to) {
    return prefix[static_cast<std::size_t>(to)] - prefix[static_cast<std::size_t>(from) - 1];
  };
  std::int64_t total = 0;
  for (const Window& w : windows) {
    total += static_cast<std::int64_t>(grid.size()) * stepsIn(w.fitFrom, w.fitTo) + stepsIn(w.testFrom, w.testTo);
  }
  ProgressMeter meter(progress, total);

  WalkForwardResult result;
  std::vector<double> testReturns;
  for (std::size_t k = 0; k < windows.size(); ++k) {
    const Window& w = windows[k];
    const auto fitTimeline = std::make_shared<const Timeline>(timeline->slice(w.fitFrom, w.fitTo));
    const std::int64_t fitSteps = fitTimeline->totalSteps();

    // Строго «больше»: при равенстве остаётся более ранний, то есть лексикографически меньший набор.
    const dsl::ParamValues* best = nullptr;
    double bestReturn = 0;
    for (const dsl::ParamValues& values : grid) {
      const RunOutput fit = runBacktest(spec, fitTimeline, program, params, values, dsl::Level::Ok, meter.forRun());
      meter.finishRun(fitSteps);
      if (fit.status == RunStatus::Invalid) continue;  // E-RT01: прогон не засчитывается
      const double r = netReturnOf(fit, spec.initialCash);
      if (best == nullptr || r > bestReturn) {
        best = &values;
        bestReturn = r;
      }
    }
    if (best == nullptr) {
      throw std::runtime_error("walk-forward: все прогоны подбора остановлены нарушением целостности (E-RT01)");
    }

    const auto testTimeline = std::make_shared<const Timeline>(timeline->slice(w.testFrom, w.testTo));
    WalkForwardSegment segment;
    segment.index = static_cast<std::int32_t>(k);
    segment.fitFromDay = w.fitFrom;
    segment.fitToDay = w.fitTo;
    segment.testFromDay = w.testFrom;
    segment.testToDay = w.testTo;
    segment.chosen = *best;
    segment.fitNetReturn = bestReturn;
    segment.test = runBacktest(spec, testTimeline, program, params, *best, dsl::Level::Ok, meter.forRun());
    meter.finishRun(testTimeline->totalSteps());
    segment.testReport = computeReport(metricsInputOf(segment.test, spec.initialCash, *testTimeline));
    testReturns.push_back(netReturnOf(segment.test, spec.initialCash));
    result.segments.push_back(std::move(segment));
  }
  result.chainedReturn = chainedReturn(testReturns);
  return result;
}

}  // namespace core
