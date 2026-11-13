#include "core/market/timeline.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace core {

namespace {

constexpr std::int64_t kDayMs = 86'400'000;
constexpr std::int64_t kMoscowOffsetMs = 3 * 3'600'000;  // МСК = UTC+3 без перехода на летнее время
constexpr std::int64_t kMinGapMs = 15 * 60'000;           // разрыв внутри дня, начиная с которого - перерыв
constexpr std::int64_t kMinGapIntervals = 2;

// Дней от 1970-01-01 до даты (григорианский календарь, алгоритм Говарда Хиннанта).
std::int64_t daysFromCivil(std::int32_t y, std::int32_t m, std::int32_t d) {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const std::int64_t yoe = y - era * 400;
  const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

std::int32_t civilFromDays(std::int64_t z) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
  return static_cast<std::int32_t>(y * 10000 + m * 100 + d);
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
  std::int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

// Московская дата момента tsMs (UTC) в виде yyyymmdd.
std::int32_t moscowYmd(std::int64_t tsMs) { return civilFromDays(floorDiv(tsMs + kMoscowOffsetMs, kDayMs)); }

// Начало московских суток даты в UTC, мс.
std::int64_t moscowDayStartMs(std::int32_t ymd) {
  return daysFromCivil(ymd / 10000, ymd / 100 % 100, ymd % 100) * kDayMs - kMoscowOffsetMs;
}

bool validYmd(std::int32_t ymd) {
  const std::int32_t m = ymd / 100 % 100;
  const std::int32_t d = ymd % 100;
  return ymd >= 10000 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

}  // namespace

Timeline Timeline::build(std::vector<Instrument> instruments, const std::vector<std::vector<Bar>>& bars,
                         const std::vector<TradingDay>& calendar) {
  // Длина шага: наименьшая длительность бара (от начала до момента, когда он стал известен).
  std::int64_t intervalMs = 0;
  for (const auto& series : bars) {
    for (const Bar& b : series) {
      const std::int64_t length = b.availableAtMs - b.tsOpenMs;
      if (length > 0 && (intervalMs == 0 || length < intervalMs)) intervalMs = length;
    }
  }
  return buildWithInterval(std::move(instruments), bars, calendar, intervalMs);
}

Timeline Timeline::buildWithInterval(std::vector<Instrument> instruments, const std::vector<std::vector<Bar>>& bars,
                                     const std::vector<TradingDay>& calendar, std::int64_t intervalMs) {
  if (bars.size() != instruments.size()) {
    throw std::invalid_argument("Timeline: число серий баров не равно числу бумаг");
  }
  for (const Instrument& instrument : instruments) {
    if (instrument.lotSize <= 0) throw std::invalid_argument("Timeline: размер лота должен быть положительным");
  }
  std::vector<std::int64_t> times;
  for (const auto& series : bars) {
    for (std::size_t i = 1; i < series.size(); ++i) {
      if (series[i].tsOpenMs <= series[i - 1].tsOpenMs) {
        throw std::invalid_argument("Timeline: бары бумаги должны идти по строго возрастающему времени");
      }
    }
    for (const Bar& b : series) times.push_back(b.tsOpenMs);
  }
  std::sort(times.begin(), times.end());
  times.erase(std::unique(times.begin(), times.end()), times.end());

  Timeline timeline;
  timeline.instruments_ = std::move(instruments);
  timeline.stepYmd_.reserve(times.size());
  timeline.stepDay_.reserve(times.size());
  for (const std::int64_t t : times) {
    const std::int32_t ymd = moscowYmd(t);
    if (timeline.stepYmd_.empty() || timeline.stepYmd_.back() != ymd) ++timeline.tradingDays_;
    timeline.stepYmd_.push_back(ymd);
    timeline.stepDay_.push_back(timeline.tradingDays_);
  }

  std::vector<std::int32_t> tradingYmds;
  for (const TradingDay& day : calendar) {
    if (day.isTrading) tradingYmds.push_back(day.yyyymmdd);
  }
  std::sort(tradingYmds.begin(), tradingYmds.end());

  for (std::size_t instrument = 0; instrument < bars.size(); ++instrument) {
    const std::int64_t lotSize = timeline.instruments_[instrument].lotSize;
    const Bar* previous = nullptr;
    std::int32_t previousYmd = 0;
    for (const Bar& b : bars[instrument]) {
      const auto step = static_cast<std::int64_t>(std::lower_bound(times.begin(), times.end(), b.tsOpenMs) - times.begin());
      StepBar s;
      s.step = step;
      s.instrument = static_cast<int>(instrument);
      s.timeUnixMs = b.tsOpenMs;
      s.tradingDay = timeline.stepDay_[static_cast<std::size_t>(step)];
      s.open = b.open;
      s.high = b.high;
      s.low = b.low;
      s.close = b.close;
      s.volumeLots = b.volume / lotSize;
      const std::int32_t ymd = timeline.stepYmd_[static_cast<std::size_t>(step)];
      if (previous != nullptr) {
        if (ymd == previousYmd) {
          const std::int64_t gap = b.tsOpenMs - previous->tsOpenMs;
          s.afterHalt = gap >= kMinGapMs && gap >= kMinGapIntervals * intervalMs;
        } else {
          // Между датами есть торговый день (по календарю), а бумага в него не торговалась.
          const auto next = std::upper_bound(tradingYmds.begin(), tradingYmds.end(), previousYmd);
          s.afterHalt = next != tradingYmds.end() && *next < ymd;
        }
      }
      timeline.bars_.push_back(s);
      previous = &b;
      previousYmd = ymd;
    }
  }
  std::sort(timeline.bars_.begin(), timeline.bars_.end(), [](const StepBar& a, const StepBar& b) {
    return a.step != b.step ? a.step < b.step : a.instrument < b.instrument;
  });
  timeline.buildIndex();
  return timeline;
}

Timeline Timeline::load(BarSource& barSource, CalendarSource& calendarSource, const std::string& snapshotId,
                        std::vector<Instrument> instruments, Timeframe timeframe, std::int32_t fromYmd,
                        std::int32_t toYmd) {
  if (!validYmd(fromYmd) || !validYmd(toYmd) || fromYmd > toYmd) {
    throw std::invalid_argument("Timeline::load: неверный диапазон дат");
  }
  const std::int32_t interval = intervalSeconds(timeframe);
  BarRequest request;
  request.snapshotId = snapshotId;
  request.intervalSec = interval;
  request.fromMs = moscowDayStartMs(fromYmd);
  request.toMs = moscowDayStartMs(toYmd) + kDayMs;  // начало суток после toYmd
  std::vector<std::vector<Bar>> bars;
  bars.reserve(instruments.size());
  for (const Instrument& instrument : instruments) {
    request.secid = instrument.ticker;
    bars.push_back(barSource.load(request));
  }
  const std::vector<TradingDay> calendar = calendarSource.tradingDays(snapshotId, fromYmd, toYmd);
  return buildWithInterval(std::move(instruments), bars, calendar, static_cast<std::int64_t>(interval) * 1000);
}

Timeline Timeline::loadWithWarmup(BarSource& barSource, CalendarSource& calendarSource, const std::string& snapshotId,
                                  std::vector<Instrument> instruments, Timeframe timeframe, std::int32_t fromYmd,
                                  std::int32_t toYmd, std::int64_t warmupSteps) {
  Timeline timeline = load(barSource, calendarSource, snapshotId, instruments, timeframe, fromYmd, toYmd);
  if (warmupSteps <= 0 || timeline.totalSteps() == 0) return timeline;  // пустой период - ошибка вызывающего
  const std::int64_t stepsPerDay =
      timeline.tradingDays_ > 0 ? std::max<std::int64_t>(1, (timeline.totalSteps() + timeline.tradingDays_ - 1) /
                                                                timeline.tradingDays_)
                                : 1;
  const std::int64_t start = daysFromCivil(fromYmd / 10000, fromYmd / 100 % 100, fromYmd % 100);
  // Не раньше 1970-01-01: даты хранилища (ClickHouse Date) начинаются с неё.
  const std::int64_t maxLookback = std::min(kMaxWarmupLookbackDays, start);
  if (maxLookback <= 0) return timeline;
  // Торговых дней в календарной неделе 5 из 7; запас 10 дней - на праздники.
  std::int64_t lookback = (warmupSteps + stepsPerDay - 1) / stepsPerDay * 7 / 5 + 10;
  for (;;) {
    lookback = std::min(lookback, maxLookback);
    const Timeline history = load(barSource, calendarSource, snapshotId, instruments, timeframe,
                                  civilFromDays(start - lookback), civilFromDays(start - 1));
    if (history.totalSteps() >= warmupSteps || lookback >= maxLookback) {
      return timeline.withWarmup(history, warmupSteps);
    }
    lookback *= 2;
  }
}

void Timeline::buildIndex() {
  const std::size_t steps = stepDay_.size();
  stepStart_.assign(steps + 1, 0);
  for (const StepBar& b : bars_) ++stepStart_[static_cast<std::size_t>(b.step) + 1];
  for (std::size_t i = 0; i < steps; ++i) stepStart_[i + 1] += stepStart_[i];
  byInstrument_.assign(instruments_.size(), {});
  for (std::size_t i = 0; i < bars_.size(); ++i) {
    byInstrument_[static_cast<std::size_t>(bars_[i].instrument)].push_back(i);
  }
}

void Timeline::checkStep(std::int64_t step) const {
  if (step < 0 || step >= totalSteps()) throw std::out_of_range("Timeline: шаг вне шкалы");
}

void Timeline::checkInstrument(int instrument) const {
  if (instrument < 0 || static_cast<std::size_t>(instrument) >= instruments_.size()) {
    throw std::out_of_range("Timeline: нет такой бумаги");
  }
}

std::span<const StepBar> Timeline::barsAt(std::int64_t step) const {
  checkStep(step);
  const auto i = static_cast<std::size_t>(step);
  return std::span<const StepBar>(bars_).subspan(stepStart_[i], stepStart_[i + 1] - stepStart_[i]);
}

const StepBar* Timeline::barOf(int instrument, std::int64_t step) const {
  checkInstrument(instrument);
  for (const StepBar& b : barsAt(step)) {
    if (b.instrument == instrument) return &b;
  }
  return nullptr;
}

const StepBar* Timeline::lastBarOf(int instrument, std::int64_t upToStep) const {
  checkInstrument(instrument);
  if (upToStep >= totalSteps()) throw std::out_of_range("Timeline: шаг вне шкалы");
  const auto& indexes = byInstrument_[static_cast<std::size_t>(instrument)];
  const auto it = std::upper_bound(indexes.begin(), indexes.end(), upToStep,
                                   [this](std::int64_t step, std::size_t index) { return step < bars_[index].step; });
  if (it == indexes.begin()) return nullptr;
  return &bars_[*(it - 1)];
}

std::int32_t Timeline::tradingDayOf(std::int64_t step) const {
  checkStep(step);
  return stepDay_[static_cast<std::size_t>(step)];
}

std::int32_t Timeline::yyyymmddOf(std::int64_t step) const {
  checkStep(step);
  return stepYmd_[static_cast<std::size_t>(step)];
}

bool Timeline::isLastStepOfDay(std::int64_t step) const {
  checkStep(step);
  const auto i = static_cast<std::size_t>(step);
  return i + 1 == stepDay_.size() || stepDay_[i + 1] != stepDay_[i];
}

Timeline Timeline::slice(std::int32_t fromDay, std::int32_t toDay) const {
  if (fromDay < 1 || toDay > tradingDays_ || fromDay > toDay) {
    throw std::invalid_argument("Timeline::slice: дни вне шкалы");
  }
  // Дни идут подряд, поэтому шаги выбранных дней - непрерывный диапазон [first, last).
  const auto first = static_cast<std::size_t>(std::lower_bound(stepDay_.begin(), stepDay_.end(), fromDay) - stepDay_.begin());
  const auto last = static_cast<std::size_t>(std::upper_bound(stepDay_.begin(), stepDay_.end(), toDay) - stepDay_.begin());

  Timeline out;
  out.instruments_ = instruments_;
  out.tradingDays_ = toDay - fromDay + 1;
  out.stepDay_.reserve(last - first);
  out.stepYmd_.assign(stepYmd_.begin() + static_cast<std::ptrdiff_t>(first), stepYmd_.begin() + static_cast<std::ptrdiff_t>(last));
  for (std::size_t i = first; i < last; ++i) out.stepDay_.push_back(stepDay_[i] - (fromDay - 1));
  out.bars_.assign(bars_.begin() + static_cast<std::ptrdiff_t>(stepStart_[first]),
                   bars_.begin() + static_cast<std::ptrdiff_t>(stepStart_[last]));
  for (StepBar& b : out.bars_) {
    b.step -= static_cast<std::int64_t>(first);
    b.tradingDay -= fromDay - 1;
  }
  // Прогрев среза: W шагов перед first - из прогрева шкалы (шаги < 0) и её дней до fromDay.
  if (warmupSteps_ > 0) {
    const auto from = static_cast<std::int64_t>(first) - warmupSteps_;
    for (const StepBar& b : warmup_) {
      if (b.step >= from) out.warmup_.push_back(b);
    }
    out.warmup_.insert(out.warmup_.end(), bars_.begin(), bars_.begin() + static_cast<std::ptrdiff_t>(stepStart_[first]));
    std::erase_if(out.warmup_, [from](const StepBar& b) { return b.step < from; });
    for (StepBar& b : out.warmup_) {
      b.step -= static_cast<std::int64_t>(first);
      b.tradingDay -= fromDay - 1;
    }
    out.warmupSteps_ = warmupSteps_;  // перед first всегда есть W шагов: свои дни шкалы и её прогрев
  }
  out.buildIndex();
  return out;
}

Timeline Timeline::withWarmup(const Timeline& history, std::int64_t steps) const {
  if (steps < 0) throw std::invalid_argument("Timeline::withWarmup: число шагов прогрева отрицательно");
  if (history.instruments_.size() != instruments_.size()) {
    throw std::invalid_argument("Timeline::withWarmup: бумаги истории не совпадают с бумагами шкалы");
  }
  for (std::size_t i = 0; i < instruments_.size(); ++i) {
    if (history.instruments_[i].key != instruments_[i].key) {
      throw std::invalid_argument("Timeline::withWarmup: бумаги истории не совпадают с бумагами шкалы");
    }
  }
  Timeline out = *this;
  out.warmup_.clear();
  const std::int64_t total = history.totalSteps();
  const std::int64_t n = std::min(steps, total);
  out.warmupSteps_ = n;
  if (n == 0) return out;
  const auto firstStep = static_cast<std::size_t>(total - n);
  out.warmup_.assign(history.bars_.begin() + static_cast<std::ptrdiff_t>(history.stepStart_[firstStep]),
                     history.bars_.end());
  const std::int32_t lastDay = history.stepDay_.back();
  for (StepBar& b : out.warmup_) {
    b.step -= total;
    b.tradingDay -= lastDay;
  }
  if (!out.warmup_.empty() && !bars_.empty()) {
    std::int64_t latest = out.warmup_.front().timeUnixMs;
    for (const StepBar& b : out.warmup_) latest = std::max(latest, b.timeUnixMs);
    if (latest >= bars_.front().timeUnixMs) {
      throw std::invalid_argument("Timeline::withWarmup: история не раньше начала шкалы");
    }
  }
  return out;
}

StepBar& Timeline::mutableBar(std::int64_t step, int instrument) {
  const StepBar* found = barOf(instrument, step);
  if (found == nullptr) throw std::out_of_range("Timeline: у бумаги нет бара на этом шаге");
  return bars_[static_cast<std::size_t>(found - bars_.data())];
}

}  // namespace core
