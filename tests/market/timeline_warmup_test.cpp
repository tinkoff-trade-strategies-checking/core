#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/market/calendar_source.hpp"
#include "core/market/date_utils.hpp"
#include "core/market/in_memory_bar_source.hpp"
#include "core/market/timeline.hpp"
#include "support/fixture1.hpp"

// Прогрев шкалы времени (MarketSpec.warmup_bars): история до начала периода с
// шагами -W…-1.

namespace core {
namespace {

using testing::fixture1;

// fixture-1 с 15 января: дни 15…22 - шесть шагов; раньше - 9, 10, 11, 12 января
// (у B нет бара 12-го).
Timeline fromJan15(std::int64_t Warmup) {
  auto F = fixture1();
  return Timeline::loadWithWarmup(F.Bars, F.Calendar, F.SnapshotId,
                                  F.Instruments, Timeframe::D1, 20240115,
                                  20240122, Warmup);
}

TEST(TimelineWarmup, LoadTakesLastStepsBeforePeriod) {
  const Timeline T = fromJan15(3);
  EXPECT_EQ(T.totalSteps(), 6);
  EXPECT_EQ(T.tradingDays(), 6);
  EXPECT_EQ(T.warmupSteps(), 3);
  // 10, 11, 12 января: A на всех трёх, B - на двух первых.
  const auto Warmup = T.warmupBars();
  ASSERT_EQ(Warmup.size(), 5u);
  const std::int64_t Steps[] = {-3, -3, -2, -2, -1};
  const int Instruments[] = {0, 1, 0, 1, 0};
  const std::int32_t Days[] = {-2, -2, -1, -1, 0};
  for (std::size_t I = 0; I < Warmup.size(); ++I) {
    EXPECT_EQ(Warmup[I].Step, Steps[I]) << I;
    EXPECT_EQ(Warmup[I].Instrument, Instruments[I]) << I;
    EXPECT_EQ(Warmup[I].TradingDay, Days[I]) << I;
  }
  EXPECT_EQ(Warmup[0].Close, Decimal::parse("101"));
  EXPECT_EQ(Warmup[4].Close, Decimal::parse("106"));
  // Сам период не изменился: шаг 0 - 15 января, close A 104.
  EXPECT_EQ(T.barOf(0, 0)->Close, Decimal::parse("104"));
  EXPECT_EQ(T.barOf(0, 0)->TradingDay, 1);
}

TEST(TimelineWarmup, ShorterHistoryTakesWhatExists) {
  const Timeline T = fromJan15(50);
  EXPECT_EQ(T.warmupSteps(),
            4); // в снапшоте до 15 января только четыре торговых дня
  EXPECT_EQ(T.warmupBars().front().Step, -4);
  EXPECT_EQ(T.warmupBars().front().Close, Decimal::parse("100"));
}

TEST(TimelineWarmup, ZeroIsPlainLoad) {
  auto F = fixture1();
  const Timeline Plain =
      Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                     Timeframe::D1, 20240115, 20240122);
  const Timeline T = fromJan15(0);
  EXPECT_EQ(T.warmupSteps(), 0);
  EXPECT_TRUE(T.warmupBars().empty());
  ASSERT_EQ(T.totalSteps(), Plain.totalSteps());
  for (std::int64_t S = 0; S < T.totalSteps(); ++S) {
    ASSERT_EQ(T.barsAt(S).size(), Plain.barsAt(S).size());
    for (std::size_t I = 0; I < T.barsAt(S).size(); ++I)
      EXPECT_EQ(T.barsAt(S)[I], Plain.barsAt(S)[I]);
  }
}

// Срез берёт W шагов прямо перед своим первым днём: из дней шкалы, а в начале -
// из её прогрева.
TEST(TimelineWarmup, SliceKeepsWarmupFromPrecedingSteps) {
  auto F = fixture1();
  // 11…22 января (8 дней), прогрев 2 шага - 9 и 10 января.
  const Timeline T =
      Timeline::loadWithWarmup(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                               Timeframe::D1, 20240111, 20240122, 2);
  ASSERT_EQ(T.warmupSteps(), 2);

  const Timeline Middle =
      T.slice(3, 5); // 15…17 января; прогрев - 11 и 12 января
  EXPECT_EQ(Middle.warmupSteps(), 2);
  ASSERT_EQ(Middle.warmupBars().size(), 3u); // у B нет бара 12-го
  EXPECT_EQ(Middle.warmupBars()[0].Step, -2);
  EXPECT_EQ(Middle.warmupBars()[0].TradingDay, -1);
  EXPECT_EQ(Middle.warmupBars()[0].Close, Decimal::parse("103"));
  EXPECT_EQ(Middle.warmupBars()[2].Step, -1);
  EXPECT_EQ(Middle.warmupBars()[2].TradingDay, 0);
  EXPECT_EQ(Middle.warmupBars()[2].Close, Decimal::parse("106"));

  const Timeline Second =
      T.slice(2, 3); // прогрев - 10 января (прогрев шкалы) и 11 января (день 1)
  ASSERT_EQ(Second.warmupSteps(), 2);
  EXPECT_EQ(Second.warmupBars().front().Close, Decimal::parse("101"));
  ASSERT_EQ(Second.warmupBars().size(), 4u);
  EXPECT_EQ(Second.warmupBars().back().Instrument, 1); // B 11 января
  EXPECT_EQ(Second.warmupBars().back().Close, Decimal::parse("0.02"));
  EXPECT_EQ(Second.warmupBars().back().Step, -1);

  // Без прогрева срез, как и раньше, начинается без истории.
  const Timeline Plain =
      Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                     Timeframe::D1, 20240111, 20240122);
  EXPECT_EQ(Plain.slice(3, 5).warmupSteps(), 0);
}

TEST(TimelineWarmup, WithWarmupChecksInput) {
  auto F = fixture1();
  const Timeline Period =
      Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                     Timeframe::D1, 20240115, 20240122);
  const Timeline Before =
      Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                     Timeframe::D1, 20240109, 20240112);
  EXPECT_THROW((void)Period.withWarmup(Before, -1), std::invalid_argument);
  // История, которая не раньше периода, - ошибка.
  EXPECT_THROW((void)Period.withWarmup(Period, 2), std::invalid_argument);
  // Другие бумаги - ошибка.
  const std::vector<Instrument> OnlyA{F.Instruments[0]};
  const Timeline OtherInstruments =
      Timeline::load(F.Bars, F.Calendar, F.SnapshotId, OnlyA, Timeframe::D1,
                     20240109, 20240112);
  EXPECT_THROW((void)Period.withWarmup(OtherInstruments, 2),
               std::invalid_argument);
  // Повторный прогрев заменяет прежний.
  const Timeline Twice = Period.withWarmup(Before, 3).withWarmup(Before, 1);
  EXPECT_EQ(Twice.warmupSteps(), 1);
  EXPECT_EQ(Twice.warmupBars().size(), 1u); // 12 января - только A
}

TEST(TimelineWarmup, DefaultWarmup200Bars) {
  static_assert(Timeline::kDefaultWarmupBars == 200);
  constexpr std::int64_t kDayMs = 86'400'000;
  const Instrument Inst{"A", "WARM", 10, Decimal::parse("0.01")};
  const Security Sec{"WARM", "Finance", 10, Decimal::parse("0.01")};
  InMemoryBarSource Bars;
  InMemoryCalendarSource Cal;
  std::vector<Bar> Series;
  std::vector<TradingDay> Days;
  // 250 торговых дней подряд (хватает на 200 баров прогрева + 50 баров сессии).
  const std::int64_t StartDay = daysFromCivil(2023, 1, 1);
  std::int64_t Offset = 0;
  std::int32_t PeriodFromYmd = 0;
  std::int32_t PeriodToYmd = 0;
  for (int I = 0; I < 250; ++I) {
    // Пропускаем выходные для реалистичного календаря (5 из 7).
    while ((StartDay + Offset + 4) % 7 >= 5)
      ++Offset;
    const std::int32_t Ymd = civilFromDays(StartDay + Offset);
    ++Offset;
    if (I == 205)
      PeriodFromYmd = Ymd;
    if (I == 249)
      PeriodToYmd = Ymd;
    Bar B;
    B.TsOpenMs = moscowDayStartMs(Ymd);
    B.AvailableAtMs = B.TsOpenMs + kDayMs;
    B.Open = Decimal::fromUnits(100 + I);
    B.High = B.Open;
    B.Low = B.Open;
    B.Close = B.Open;
    B.Volume = 1000;
    Series.push_back(B);
    Days.push_back(TradingDay{Ymd, true});
  }
  Bars.addSecurity("snap-200", Sec);
  Bars.addSeries("snap-200", "WARM", intervalSeconds(Timeframe::D1), Series);
  Cal.add("snap-200", Days);

  const Timeline T = Timeline::loadWithWarmup(
      Bars, Cal, "snap-200", {Inst}, Timeframe::D1, PeriodFromYmd, PeriodToYmd,
      Timeline::kDefaultWarmupBars);
  EXPECT_EQ(T.warmupSteps(), 200);
  ASSERT_EQ(T.warmupBars().size(), 200u);
  EXPECT_EQ(T.warmupBars().front().Step, -200);
  EXPECT_EQ(T.warmupBars().back().Step, -1);
  EXPECT_EQ(T.totalSteps(), 45);
}

} // namespace
} // namespace core
