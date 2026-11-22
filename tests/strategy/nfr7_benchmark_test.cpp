#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/engine/journal.hpp"
#include "core/engine/session_engine.hpp"
#include "core/exec/costs.hpp"
#include "core/market/date_utils.hpp"
#include "core/market/instrument.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/strategy/strategy_runner.hpp"

namespace {

using namespace core;

const char *const kReferenceStrategy = R"dsl(
# Эталон платформы: пересечение скользящих средних
param fast = 20 range 5..50 step 5
param slow = 50 range 20..200 step 10
param rsi_n = 14
param rsi_high = 70 range 60..85 step 5
param stop_pct = 5 range 2..10 step 1
param size_pct = 95

rule "вход"
  when crossover(sma(close, fast), sma(close, slow)) and position == 0
  then buy size_pct % cash
  because "SMA({fast}) пересекла SMA({slow}) снизу вверх"

rule "стоп"
  when position > 0
  then sell all stop entry_price * (1 - stop_pct / 100)
  because "защитный стоп {stop_pct}% от цены входа {entry_price}"

rule "перекупленность"
  when position > 0 and rsi(close, rsi_n) > rsi_high
  then sell all
  because "перекупленность, RSI = {rsi(close, rsi_n)}"

rule "выход"
  when position > 0 and crossunder(sma(close, fast), sma(close, slow))
  then sell all
  because "SMA({fast}) опустилась ниже SMA({slow})"
)dsl";

Decimal centsToDecimal(std::int64_t Cents) {
  return Decimal::fromRaw(static_cast<Int128>(Cents) * (Decimal::kUnit / 100));
}

// Треугольная волна вокруг 100.00..130.00 без double, чтобы пересечения SMA и
// RSI срабатывали регулярно по всей длине истории.
std::int64_t waveCents(std::int64_t Index, std::size_t InstrIdx) {
  const std::int64_t Period = 120;
  const std::int64_t Phase =
      (Index + static_cast<std::int64_t>(InstrIdx) * 19) % Period;
  const std::int64_t Half = Period / 2;
  const std::int64_t Saw = (Phase < Half) ? Phase : (Period - Phase);
  return 10000 + Saw * 50 + static_cast<std::int64_t>(InstrIdx) * 200;
}

std::shared_ptr<const Timeline>
buildBenchmarkTimeline(std::size_t NumInstruments, std::int64_t TradingSteps,
                       std::int64_t WarmupSteps, std::int64_t StepMs,
                       std::int64_t StepsPerDay) {
  std::vector<Instrument> Instruments;
  Instruments.reserve(NumInstruments);
  for (std::size_t I = 0; I < NumInstruments; ++I) {
    Instruments.push_back(Instrument{"K" + std::to_string(I),
                                     "TICK" + std::to_string(I), 1,
                                     Decimal::parse("0.01")});
  }

  const std::int64_t TotalSteps = WarmupSteps + TradingSteps;
  const std::int64_t TotalDays = (TotalSteps + StepsPerDay - 1) / StepsPerDay;
  std::vector<TradingDay> Calendar;
  Calendar.reserve(static_cast<std::size_t>(TotalDays));
  std::int32_t Ymd = 20220103;
  for (std::int64_t D = 0; D < TotalDays; ++D) {
    Calendar.push_back(TradingDay{Ymd, true});
    Ymd = addCalendarDays(Ymd, 1);
  }

  std::vector<std::vector<Bar>> WarmupBars(NumInstruments);
  std::vector<std::vector<Bar>> TradingBars(NumInstruments);
  for (std::size_t I = 0; I < NumInstruments; ++I) {
    WarmupBars[I].reserve(static_cast<std::size_t>(WarmupSteps));
    TradingBars[I].reserve(static_cast<std::size_t>(TradingSteps));
  }

  for (std::int64_t S = 0; S < TotalSteps; ++S) {
    const std::int64_t DayIdx = S / StepsPerDay;
    const std::int64_t InDayIdx = S % StepsPerDay;
    const std::int64_t OpenMs =
        moscowDayStartMs(Calendar[static_cast<std::size_t>(DayIdx)].Yyyymmdd) +
        10 * 3600'000LL + InDayIdx * StepMs;
    for (std::size_t I = 0; I < NumInstruments; ++I) {
      const std::int64_t PrevCents = waveCents(S > 0 ? S - 1 : 0, I);
      const std::int64_t CurrCents = waveCents(S, I);
      const std::int64_t HighCents = std::max(PrevCents, CurrCents) + 80;
      const std::int64_t LowCents =
          std::max<std::int64_t>(100, std::min(PrevCents, CurrCents) - 80);

      Bar B;
      B.TsOpenMs = OpenMs;
      B.AvailableAtMs = OpenMs + StepMs;
      B.Open = centsToDecimal(PrevCents);
      B.High = centsToDecimal(HighCents);
      B.Low = centsToDecimal(LowCents);
      B.Close = centsToDecimal(CurrCents);
      B.Volume = 100000;
      B.Value = B.Close.times(B.Volume);
      if (S < WarmupSteps) {
        WarmupBars[I].push_back(B);
      } else {
        TradingBars[I].push_back(B);
      }
    }
  }

  Timeline MainTl = Timeline::build(Instruments, TradingBars, Calendar);
  if (WarmupSteps > 0) {
    const Timeline Hist = Timeline::build(Instruments, WarmupBars, Calendar);
    MainTl = MainTl.withWarmup(Hist, WarmupSteps);
  }
  return std::make_shared<const Timeline>(std::move(MainTl));
}

SessionSpec makeBenchSpec(const Timeline &Tl) {
  SessionSpec Spec;
  Spec.Instruments = Tl.instruments();
  Spec.InitialCash = Decimal::parse("10000000");
  Spec.Costs.CommissionRate = Decimal::parse("0.0005");
  Spec.Costs.ExchangeFeeRate = Decimal::parse("0.0001");
  Spec.Costs.Spread = Decimal::parse("0.001");
  Spec.Costs.SlippageKappa = Decimal::parse("0.1");
  return Spec;
}

struct LatencyStats {
  double P50Ms = 0.0;
  double P95Ms = 0.0;
  double MaxMs = 0.0;
};

LatencyStats computeStats(std::vector<double> SamplesMs) {
  LatencyStats Stats;
  if (SamplesMs.empty()) {
    return Stats;
  }
  std::sort(SamplesMs.begin(), SamplesMs.end());
  const auto Percentile = [&](double Q) {
    const double Pos = Q * static_cast<double>(SamplesMs.size() - 1);
    const auto Idx = static_cast<std::size_t>(Pos);
    return SamplesMs[Idx];
  };
  Stats.P50Ms = Percentile(0.50);
  Stats.P95Ms = Percentile(0.95);
  Stats.MaxMs = SamplesMs.back();
  return Stats;
}

LatencyStats measureAdvanceSteps(const SessionSpec &Spec,
                                 const std::shared_ptr<const Timeline> &Tl) {
  SessionEngine Engine(Spec, Tl);
  std::vector<double> StepTimesMs;
  StepTimesMs.reserve(static_cast<std::size_t>(Tl->totalSteps()));
  std::int64_t Seq = 0;
  while (Engine.status() == SessionStatus::Running &&
         Engine.currentStep() + 1 < Tl->totalSteps()) {
    const std::int64_t Step = Engine.currentStep();
    if (Step % 25 == 0) {
      JournalEntry Entry;
      Entry.Seq = Seq++;
      Entry.Step = Step;
      OrderSpec Ord;
      Ord.Instrument = static_cast<int>((Step / 25) % Spec.Instruments.size());
      Ord.OrderSide = ((Step / 25) % 2 == 0) ? Side::Buy : Side::Sell;
      Ord.Type = OrderType::Market;
      Ord.Lots = 1;
      Ord.Tif = TimeInForce::Gtc;
      Entry.Command = PlaceCmd{Ord, std::nullopt};
      (void)Engine.apply(Entry);
    }
    const auto T0 = std::chrono::steady_clock::now();
    Engine.advance();
    const auto T1 = std::chrono::steady_clock::now();
    StepTimesMs.push_back(
        std::chrono::duration<double, std::milli>(T1 - T0).count());
  }
  return computeStats(std::move(StepTimesMs));
}

void runAndReportScenario(const char *Label, std::size_t NumInstruments,
                          std::int64_t TradingSteps, std::int64_t WarmupSteps,
                          std::int64_t StepMs, std::int64_t StepsPerDay) {
  const auto Tl = buildBenchmarkTimeline(NumInstruments, TradingSteps,
                                         WarmupSteps, StepMs, StepsPerDay);
  const SessionSpec Spec = makeBenchSpec(*Tl);

  const LatencyStats StepStats = measureAdvanceSteps(Spec, Tl);
  EXPECT_LE(StepStats.P95Ms, 300.0);

  StrategyRunOptions Opts;
  Opts.Source = kReferenceStrategy;
  Opts.ParamOverrides["size_pct"] =
      static_cast<double>(95 / static_cast<int>(NumInstruments));
  Opts.TimeBudget = std::chrono::minutes(10);

  const auto RunT0 = std::chrono::steady_clock::now();
  const StrategyRunResult Run = runStrategy(Spec, Tl, Opts);
  const auto RunT1 = std::chrono::steady_clock::now();
  const double RunMs =
      std::chrono::duration<double, std::milli>(RunT1 - RunT0).count();

  ASSERT_TRUE(Run.Compiled);
  EXPECT_FALSE(Run.Cancelled);
  EXPECT_FALSE(Run.Invalid);
  ASSERT_TRUE(Run.Engine.has_value());
  EXPECT_FALSE(Run.Journal.empty());
  EXPECT_FALSE(Run.Engine->fills().empty());
  EXPECT_LT(RunMs, 600000.0);

  const double StepsPerSec =
      static_cast<double>(TradingSteps) / (RunMs / 1000.0);
  std::cout << std::fixed << std::setprecision(4) << "[NFR-7 " << Label << "] "
            << NumInstruments << " instr x " << TradingSteps << " steps (+"
            << WarmupSteps << " warmup): advance p50=" << StepStats.P50Ms
            << " ms, p95=" << StepStats.P95Ms << " ms, max=" << StepStats.MaxMs
            << " ms; runStrategy=" << RunMs << " ms (" << std::setprecision(0)
            << StepsPerSec << " steps/s, orders=" << Run.Engine->orders().size()
            << ", fills=" << Run.Engine->fills().size() << ")\n";
}

} // namespace

TEST(Nfr7Benchmark, PlayerStepAndRunStrategyWithinNfr7) {
  // Быстрый CI-бенчмарк (< 2s даже в Debug): 5 бумаг x 1000 торговых баров +
  // 200 баров прогрева с эталонной стратегией платформы.
  runAndReportScenario("5x1000", 5, 1000, Timeline::kDefaultWarmupBars,
                       60'000LL, 520);
}

TEST(Nfr7Benchmark, DISABLED_FullArchive250Days) {
  // Снятие цифр НФТ-7 в Release: 5 бумаг за 250 торговых дней (M5 и M1), а
  // также максимальная ручная сессия из 5000 шагов (replaySession).
  runAndReportScenario("250d-M5", 5, 250LL * 104, Timeline::kDefaultWarmupBars,
                       300'000LL, 104);
  runAndReportScenario("250d-M1", 5, 250LL * 520, Timeline::kDefaultWarmupBars,
                       60'000LL, 520);

  const auto ManualTl = buildBenchmarkTimeline(
      5, 5000, Timeline::kDefaultWarmupBars, 60'000LL, 520);
  const SessionSpec ManualSpec = makeBenchSpec(*ManualTl);
  std::vector<JournalEntry> Journal;
  for (int I = 0; I < 20; ++I) {
    JournalEntry Entry;
    Entry.Seq = I;
    Entry.Step = static_cast<std::int64_t>(I) * 200;
    OrderSpec Ord;
    Ord.Instrument = I % 5;
    Ord.OrderSide = (I % 2 == 0) ? Side::Buy : Side::Sell;
    Ord.Type = OrderType::Market;
    Ord.Lots = 1;
    Ord.Tif = TimeInForce::Gtc;
    Entry.Command = PlaceCmd{Ord, std::nullopt};
    Journal.push_back(Entry);
  }
  std::vector<double> ReplaySamplesMs;
  for (int I = 0; I < 20; ++I) {
    const std::int64_t UpTo = 4980 + I;
    const auto T0 = std::chrono::steady_clock::now();
    const ReplayResult Res =
        replaySession(ManualSpec, ManualTl, Journal, UpTo, UpTo - 1);
    const auto T1 = std::chrono::steady_clock::now();
    EXPECT_EQ(Res.LastStep, UpTo);
    ReplaySamplesMs.push_back(
        std::chrono::duration<double, std::milli>(T1 - T0).count());
  }
  const LatencyStats ReplayStats = computeStats(std::move(ReplaySamplesMs));
  EXPECT_LE(ReplayStats.P95Ms, 300.0);
  std::cout << std::fixed << std::setprecision(4)
            << "[NFR-7 5000-replay] p50=" << ReplayStats.P50Ms
            << " ms, p95=" << ReplayStats.P95Ms
            << " ms, max=" << ReplayStats.MaxMs << " ms\n";
}
