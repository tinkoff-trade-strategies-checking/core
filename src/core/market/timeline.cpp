#include "core/market/timeline.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace core {

namespace {

constexpr std::int64_t kDayMs = 86'400'000;
constexpr std::int64_t kHourMs = 3'600'000;
constexpr std::int64_t kMoscowUtc3Ms = 3 * kHourMs;
constexpr std::int64_t kMoscowUtc4Ms = 4 * kHourMs;
constexpr std::int64_t kMinGapMs =
    15 * 60'000; // разрыв внутри дня, начиная с которого - перерыв
constexpr std::int64_t kMinGapIntervals = 2;

// Дней от 1970-01-01 до даты (григорианский календарь, алгоритм Говарда
// Хиннанта).
constexpr std::int64_t daysFromCivil(std::int32_t Y, std::int32_t M,
                                     std::int32_t D) {
  Y -= M <= 2 ? 1 : 0;
  const std::int64_t Era = (Y >= 0 ? Y : Y - 399) / 400;
  const std::int64_t Yoe = Y - Era * 400;
  const std::int64_t Doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
  const std::int64_t Doe = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;
  return Era * 146097 + Doe - 719468;
}

std::int32_t civilFromDays(std::int64_t Z) {
  Z += 719468;
  const std::int64_t Era = (Z >= 0 ? Z : Z - 146096) / 146097;
  const std::int64_t Doe = Z - Era * 146097;
  const std::int64_t Yoe =
      (Doe - Doe / 1460 + Doe / 36524 - Doe / 146096) / 365;
  const std::int64_t Doy = Doe - (365 * Yoe + Yoe / 4 - Yoe / 100);
  const std::int64_t Mp = (5 * Doy + 2) / 153;
  const std::int64_t D = Doy - (153 * Mp + 2) / 5 + 1;
  const std::int64_t M = Mp < 10 ? Mp + 3 : Mp - 9;
  const std::int64_t Y = Yoe + Era * 400 + (M <= 2 ? 1 : 0);
  return static_cast<std::int32_t>(Y * 10000 + M * 100 + D);
}

std::int64_t floorDiv(std::int64_t A, std::int64_t B) {
  std::int64_t Q = A / B;
  if ((A % B != 0) && ((A < 0) != (B < 0)))
    --Q;
  return Q;
}

// Номер дня последнего воскресенья месяца (для марта и октября - 31 день).
std::int64_t lastSundayDays(std::int32_t Year, std::int32_t Month) {
  const std::int64_t Last = daysFromCivil(Year, Month, 31);
  const std::int64_t Wday = ((Last + 4) % 7 + 7) % 7; // 0 = воскресенье
  return Last - Wday;
}

// Смещение МСК относительно UTC в момент TsMs (мс) без системной базы tz
// (INV-4). С 2014-10-26 01:00 UTC+3 (2014-10-25 22:00 UTC) - UTC+3;
// с 2011-03-27 03:00 UTC+4 (2011-03-26 23:00 UTC) до перехода 2014-10-26 -
// UTC+4; ранее - сезонный перевод РФ (последнее воскресенье марта/октября).
// Правило точно для 1996-2010: до 1996 осенний перевод был в сентябре, поэтому
// для дат раньше 1996 смещение в сентябре определяется неверно (берётся UTC+4
// до последнего воскресенья октября). Поддерживаемый диапазон - с 1996 года.
std::int64_t moscowOffsetMs(std::int64_t TsMs) {
  constexpr std::int64_t kOct2014SwitchMs =
      daysFromCivil(2014, 10, 25) * kDayMs + 22 * kHourMs;
  if (TsMs >= kOct2014SwitchMs)
    return kMoscowUtc3Ms;

  constexpr std::int64_t kMar2011SwitchMs =
      daysFromCivil(2011, 3, 26) * kDayMs + 23 * kHourMs;
  if (TsMs >= kMar2011SwitchMs)
    return kMoscowUtc4Ms;

  const std::int32_t ApproxYmd = civilFromDays(floorDiv(TsMs, kDayMs));
  const std::int32_t Year = ApproxYmd / 10000;
  const std::int64_t SpringSwitchMs =
      (lastSundayDays(Year, 3) - 1) * kDayMs + 23 * kHourMs;
  const std::int64_t AutumnSwitchMs =
      (lastSundayDays(Year, 10) - 1) * kDayMs + 23 * kHourMs;
  if (TsMs >= SpringSwitchMs && TsMs < AutumnSwitchMs)
    return kMoscowUtc4Ms;
  return kMoscowUtc3Ms;
}

// Московская дата момента tsMs (UTC) в виде yyyymmdd.
std::int32_t moscowYmd(std::int64_t TsMs) {
  return civilFromDays(floorDiv(TsMs + moscowOffsetMs(TsMs), kDayMs));
}

// Начало московских суток даты в UTC, мс.
std::int64_t moscowDayStartMs(std::int32_t Ymd) {
  const std::int64_t Days =
      daysFromCivil(Ymd / 10000, Ymd / 100 % 100, Ymd % 100);
  return Days * kDayMs - moscowOffsetMs(Days * kDayMs - kMoscowUtc3Ms);
}

bool validYmd(std::int32_t Ymd) {
  const std::int32_t M = Ymd / 100 % 100;
  const std::int32_t D = Ymd % 100;
  return Ymd >= 10000 && M >= 1 && M <= 12 && D >= 1 && D <= 31;
}

} // namespace

Timeline Timeline::build(std::vector<Instrument> Instruments,
                         const std::vector<std::vector<Bar>> &Bars,
                         const std::vector<TradingDay> &Calendar) {
  // Длина шага: наименьшая длительность бара (от начала до момента, когда он
  // стал известен).
  std::int64_t IntervalMs = 0;
  for (const auto &Series : Bars) {
    for (const Bar &B : Series) {
      const std::int64_t Length = B.AvailableAtMs - B.TsOpenMs;
      if (Length > 0 && (IntervalMs == 0 || Length < IntervalMs))
        IntervalMs = Length;
    }
  }
  return buildWithInterval(std::move(Instruments), Bars, Calendar, IntervalMs);
}

Timeline Timeline::buildWithInterval(std::vector<Instrument> Instruments,
                                     const std::vector<std::vector<Bar>> &Bars,
                                     const std::vector<TradingDay> &Calendar,
                                     std::int64_t IntervalMs) {
  if (Bars.size() != Instruments.size()) {
    throw std::invalid_argument(
        "Timeline: число серий баров не равно числу бумаг");
  }
  for (const Instrument &Instrument : Instruments) {
    if (Instrument.LotSize <= 0)
      throw std::invalid_argument(
          "Timeline: размер лота должен быть положительным");
  }
  std::vector<std::int64_t> Times;
  for (const auto &Series : Bars) {
    for (std::size_t I = 1; I < Series.size(); ++I) {
      if (Series[I].TsOpenMs <= Series[I - 1].TsOpenMs) {
        throw std::invalid_argument("Timeline: бары бумаги должны идти по "
                                    "строго возрастающему времени");
      }
    }
    for (const Bar &B : Series)
      Times.push_back(B.TsOpenMs);
  }
  std::sort(Times.begin(), Times.end());
  Times.erase(std::unique(Times.begin(), Times.end()), Times.end());

  Timeline Timeline;
  Timeline.Instruments = std::move(Instruments);
  Timeline.StepYmd.reserve(Times.size());
  Timeline.StepDay.reserve(Times.size());
  for (const std::int64_t T : Times) {
    const std::int32_t Ymd = moscowYmd(T);
    if (Timeline.StepYmd.empty() || Timeline.StepYmd.back() != Ymd)
      ++Timeline.TradingDays;
    Timeline.StepYmd.push_back(Ymd);
    Timeline.StepDay.push_back(Timeline.TradingDays);
  }

  std::vector<std::int32_t> TradingYmds;
  for (const TradingDay &Day : Calendar) {
    if (Day.IsTrading)
      TradingYmds.push_back(Day.Yyyymmdd);
  }
  std::sort(TradingYmds.begin(), TradingYmds.end());

  for (std::size_t Instrument = 0; Instrument < Bars.size(); ++Instrument) {
    const std::int64_t LotSize = Timeline.Instruments[Instrument].LotSize;
    const Bar *Previous = nullptr;
    std::int32_t PreviousYmd = 0;
    for (const Bar &B : Bars[Instrument]) {
      const auto Step = static_cast<std::int64_t>(
          std::lower_bound(Times.begin(), Times.end(), B.TsOpenMs) -
          Times.begin());
      StepBar S;
      S.Step = Step;
      S.Instrument = static_cast<int>(Instrument);
      S.TimeUnixMs = B.TsOpenMs;
      S.TradingDay = Timeline.StepDay[static_cast<std::size_t>(Step)];
      S.Open = B.Open;
      S.High = B.High;
      S.Low = B.Low;
      S.Close = B.Close;
      S.VolumeLots = B.Volume / LotSize;
      const std::int32_t Ymd = Timeline.StepYmd[static_cast<std::size_t>(Step)];
      if (Previous != nullptr) {
        if (Ymd == PreviousYmd) {
          const std::int64_t Gap = B.TsOpenMs - Previous->TsOpenMs;
          S.AfterHalt =
              Gap >= kMinGapMs && Gap >= kMinGapIntervals * IntervalMs;
        } else {
          // Между датами есть торговый день (по календарю), а бумага в него не
          // торговалась.
          const auto Next = std::upper_bound(TradingYmds.begin(),
                                             TradingYmds.end(), PreviousYmd);
          S.AfterHalt = Next != TradingYmds.end() && *Next < Ymd;
        }
      }
      Timeline.Bars.push_back(S);
      Previous = &B;
      PreviousYmd = Ymd;
    }
  }
  std::sort(Timeline.Bars.begin(), Timeline.Bars.end(),
            [](const StepBar &A, const StepBar &B) {
              return A.Step != B.Step ? A.Step < B.Step
                                      : A.Instrument < B.Instrument;
            });
  Timeline.buildIndex();
  return Timeline;
}

Timeline Timeline::load(BarSource &BarSource, CalendarSource &CalendarSource,
                        const std::string &SnapshotId,
                        std::vector<Instrument> Instruments,
                        Timeframe Timeframe, std::int32_t FromYmd,
                        std::int32_t ToYmd) {
  if (!validYmd(FromYmd) || !validYmd(ToYmd) || FromYmd > ToYmd) {
    throw std::invalid_argument("Timeline::load: неверный диапазон дат");
  }
  const std::int32_t Interval = intervalSeconds(Timeframe);
  BarRequest Request;
  Request.SnapshotId = SnapshotId;
  Request.IntervalSec = Interval;
  Request.FromMs = moscowDayStartMs(FromYmd);
  const std::int32_t NextYmd = civilFromDays(
      daysFromCivil(ToYmd / 10000, ToYmd / 100 % 100, ToYmd % 100) + 1);
  Request.ToMs = moscowDayStartMs(NextYmd); // начало суток после toYmd
  std::vector<std::vector<Bar>> Bars;
  Bars.reserve(Instruments.size());
  for (const Instrument &Instrument : Instruments) {
    Request.Secid = Instrument.Ticker;
    Bars.push_back(BarSource.load(Request));
  }
  const std::vector<TradingDay> Calendar =
      CalendarSource.tradingDays(SnapshotId, FromYmd, ToYmd);
  return buildWithInterval(std::move(Instruments), Bars, Calendar,
                           static_cast<std::int64_t>(Interval) * 1000);
}

Timeline Timeline::loadWithWarmup(BarSource &BarSource,
                                  CalendarSource &CalendarSource,
                                  const std::string &SnapshotId,
                                  std::vector<Instrument> Instruments,
                                  Timeframe Timeframe, std::int32_t FromYmd,
                                  std::int32_t ToYmd,
                                  std::int64_t WarmupSteps) {
  Timeline Result = load(BarSource, CalendarSource, SnapshotId, Instruments,
                         Timeframe, FromYmd, ToYmd);
  if (WarmupSteps <= 0 || Result.totalSteps() == 0)
    return Result; // пустой период - ошибка вызывающего
  const std::int64_t StepsPerDay =
      Result.TradingDays > 0
          ? std::max<std::int64_t>(
                1, (Result.totalSteps() + Result.TradingDays - 1) /
                       Result.TradingDays)
          : 1;
  const std::int64_t Start =
      daysFromCivil(FromYmd / 10000, FromYmd / 100 % 100, FromYmd % 100);
  // Не раньше 1970-01-01: даты хранилища (ClickHouse Date) начинаются с неё.
  const std::int64_t MaxLookback = std::min(kMaxWarmupLookbackDays, Start);
  if (MaxLookback <= 0)
    return Result;
  // Торговых дней в календарной неделе 5 из 7; запас 10 дней - на праздники.
  std::int64_t Lookback =
      (WarmupSteps + StepsPerDay - 1) / StepsPerDay * 7 / 5 + 10;
  for (;;) {
    Lookback = std::min(Lookback, MaxLookback);
    const Timeline History =
        load(BarSource, CalendarSource, SnapshotId, Instruments, Timeframe,
             civilFromDays(Start - Lookback), civilFromDays(Start - 1));
    if (History.totalSteps() >= WarmupSteps || Lookback >= MaxLookback) {
      return Result.withWarmup(History, WarmupSteps);
    }
    Lookback *= 2;
  }
}

void Timeline::buildIndex() {
  const std::size_t Steps = StepDay.size();
  StepStart.assign(Steps + 1, 0);
  for (const StepBar &B : Bars)
    ++StepStart[static_cast<std::size_t>(B.Step) + 1];
  for (std::size_t I = 0; I < Steps; ++I)
    StepStart[I + 1] += StepStart[I];
  ByInstrument.assign(Instruments.size(), {});
  for (std::size_t I = 0; I < Bars.size(); ++I) {
    ByInstrument[static_cast<std::size_t>(Bars[I].Instrument)].push_back(I);
  }
}

void Timeline::checkStep(std::int64_t Step) const {
  if (Step < 0 || Step >= totalSteps())
    throw std::out_of_range("Timeline: шаг вне шкалы");
}

void Timeline::checkInstrument(int Instrument) const {
  if (Instrument < 0 ||
      static_cast<std::size_t>(Instrument) >= Instruments.size()) {
    throw std::out_of_range("Timeline: нет такой бумаги");
  }
}

std::span<const StepBar> Timeline::barsAt(std::int64_t Step) const {
  checkStep(Step);
  const auto I = static_cast<std::size_t>(Step);
  return std::span<const StepBar>(Bars).subspan(StepStart[I], StepStart[I + 1] -
                                                                  StepStart[I]);
}

const StepBar *Timeline::barOf(int Instrument, std::int64_t Step) const {
  checkInstrument(Instrument);
  for (const StepBar &B : barsAt(Step)) {
    if (B.Instrument == Instrument)
      return &B;
  }
  return nullptr;
}

const StepBar *Timeline::lastBarOf(int Instrument,
                                   std::int64_t UpToStep) const {
  checkInstrument(Instrument);
  if (UpToStep >= totalSteps())
    throw std::out_of_range("Timeline: шаг вне шкалы");
  const auto &Indexes = ByInstrument[static_cast<std::size_t>(Instrument)];
  const auto It =
      std::upper_bound(Indexes.begin(), Indexes.end(), UpToStep,
                       [this](std::int64_t Step, std::size_t Index) {
                         return Step < Bars[Index].Step;
                       });
  if (It == Indexes.begin())
    return nullptr;
  return &Bars[*(It - 1)];
}

std::int32_t Timeline::tradingDayOf(std::int64_t Step) const {
  checkStep(Step);
  return StepDay[static_cast<std::size_t>(Step)];
}

std::int32_t Timeline::yyyymmddOf(std::int64_t Step) const {
  checkStep(Step);
  return StepYmd[static_cast<std::size_t>(Step)];
}

std::int32_t Timeline::previousDayYmd() const {
  if (Warmup.empty())
    return 0;
  return moscowYmd(Warmup.back().TimeUnixMs);
}

bool Timeline::isLastStepOfDay(std::int64_t Step) const {
  checkStep(Step);
  const auto I = static_cast<std::size_t>(Step);
  return I + 1 == StepDay.size() || StepDay[I + 1] != StepDay[I];
}

Timeline Timeline::slice(std::int32_t FromDay, std::int32_t ToDay) const {
  if (FromDay < 1 || ToDay > TradingDays || FromDay > ToDay) {
    throw std::invalid_argument("Timeline::slice: дни вне шкалы");
  }
  // Дни идут подряд, поэтому шаги выбранных дней - непрерывный диапазон [first,
  // last).
  const auto First = static_cast<std::size_t>(
      std::lower_bound(StepDay.begin(), StepDay.end(), FromDay) -
      StepDay.begin());
  const auto Last = static_cast<std::size_t>(
      std::upper_bound(StepDay.begin(), StepDay.end(), ToDay) -
      StepDay.begin());

  Timeline Out;
  Out.Instruments = Instruments;
  Out.TradingDays = ToDay - FromDay + 1;
  Out.StepDay.reserve(Last - First);
  Out.StepYmd.assign(StepYmd.begin() + static_cast<std::ptrdiff_t>(First),
                     StepYmd.begin() + static_cast<std::ptrdiff_t>(Last));
  for (std::size_t I = First; I < Last; ++I)
    Out.StepDay.push_back(StepDay[I] - (FromDay - 1));
  Out.Bars.assign(Bars.begin() + static_cast<std::ptrdiff_t>(StepStart[First]),
                  Bars.begin() + static_cast<std::ptrdiff_t>(StepStart[Last]));
  for (StepBar &B : Out.Bars) {
    B.Step -= static_cast<std::int64_t>(First);
    B.TradingDay -= FromDay - 1;
  }
  // Прогрев среза: W шагов перед first - из прогрева шкалы (шаги < 0) и её дней
  // до fromDay.
  if (WarmupSteps > 0) {
    const auto From = static_cast<std::int64_t>(First) - WarmupSteps;
    for (const StepBar &B : Warmup) {
      if (B.Step >= From)
        Out.Warmup.push_back(B);
    }
    Out.Warmup.insert(Out.Warmup.end(), Bars.begin(),
                      Bars.begin() +
                          static_cast<std::ptrdiff_t>(StepStart[First]));
    std::erase_if(Out.Warmup,
                  [From](const StepBar &B) { return B.Step < From; });
    for (StepBar &B : Out.Warmup) {
      B.Step -= static_cast<std::int64_t>(First);
      B.TradingDay -= FromDay - 1;
    }
    Out.WarmupSteps = WarmupSteps; // перед first всегда есть W шагов: свои
                                   // дни шкалы и её прогрев
  }
  Out.buildIndex();
  return Out;
}

Timeline Timeline::withWarmup(const Timeline &History,
                              std::int64_t Steps) const {
  if (Steps < 0)
    throw std::invalid_argument(
        "Timeline::withWarmup: число шагов прогрева отрицательно");
  if (History.Instruments.size() != Instruments.size()) {
    throw std::invalid_argument(
        "Timeline::withWarmup: бумаги истории не совпадают с бумагами шкалы");
  }
  for (std::size_t I = 0; I < Instruments.size(); ++I) {
    if (History.Instruments[I].Key != Instruments[I].Key) {
      throw std::invalid_argument(
          "Timeline::withWarmup: бумаги истории не совпадают с бумагами шкалы");
    }
  }
  Timeline Out = *this;
  Out.Warmup.clear();
  const std::int64_t Total = History.totalSteps();
  const std::int64_t N = std::min(Steps, Total);
  Out.WarmupSteps = N;
  if (N == 0)
    return Out;
  const auto FirstStep = static_cast<std::size_t>(Total - N);
  Out.Warmup.assign(History.Bars.begin() + static_cast<std::ptrdiff_t>(
                                               History.StepStart[FirstStep]),
                    History.Bars.end());
  const std::int32_t LastDay = History.StepDay.back();
  for (StepBar &B : Out.Warmup) {
    B.Step -= Total;
    B.TradingDay -= LastDay;
  }
  if (!Out.Warmup.empty() && !Bars.empty()) {
    std::int64_t Latest = Out.Warmup.front().TimeUnixMs;
    for (const StepBar &B : Out.Warmup)
      Latest = std::max(Latest, B.TimeUnixMs);
    if (Latest >= Bars.front().TimeUnixMs) {
      throw std::invalid_argument(
          "Timeline::withWarmup: история не раньше начала шкалы");
    }
  }
  return Out;
}

} // namespace core
