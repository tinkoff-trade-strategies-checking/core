#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/market/timeline.hpp"
#include "support/fixture1.hpp"

// Прогрев шкалы времени (MarketSpec.warmup_bars): история до начала периода с
// шагами -W…-1.

namespace core {
namespace {

using testing::fixture1;

// fixture-1 с 15 января: дни 15…22 - шесть шагов; раньше - 9, 10, 11, 12 января
// (у B нет бара 12-го).
Timeline fromJan15(std::int64_t warmup) {
  auto f = fixture1();
  return Timeline::loadWithWarmup(f.bars, f.calendar, f.snapshotId,
                                  f.instruments, Timeframe::D1, 20240115,
                                  20240122, warmup);
}

TEST(TimelineWarmup, LoadTakesLastStepsBeforePeriod) {
  const Timeline t = fromJan15(3);
  EXPECT_EQ(t.totalSteps(), 6);
  EXPECT_EQ(t.tradingDays(), 6);
  EXPECT_EQ(t.warmupSteps(), 3);
  // 10, 11, 12 января: A на всех трёх, B - на двух первых.
  const auto warmup = t.warmupBars();
  ASSERT_EQ(warmup.size(), 5u);
  const std::int64_t steps[] = {-3, -3, -2, -2, -1};
  const int instruments[] = {0, 1, 0, 1, 0};
  const std::int32_t days[] = {-2, -2, -1, -1, 0};
  for (std::size_t i = 0; i < warmup.size(); ++i) {
    EXPECT_EQ(warmup[i].step, steps[i]) << i;
    EXPECT_EQ(warmup[i].instrument, instruments[i]) << i;
    EXPECT_EQ(warmup[i].tradingDay, days[i]) << i;
  }
  EXPECT_EQ(warmup[0].close, Decimal::parse("101"));
  EXPECT_EQ(warmup[4].close, Decimal::parse("106"));
  // Сам период не изменился: шаг 0 - 15 января, close A 104.
  EXPECT_EQ(t.barOf(0, 0)->close, Decimal::parse("104"));
  EXPECT_EQ(t.barOf(0, 0)->tradingDay, 1);
}

TEST(TimelineWarmup, ShorterHistoryTakesWhatExists) {
  const Timeline t = fromJan15(50);
  EXPECT_EQ(t.warmupSteps(),
            4); // в снапшоте до 15 января только четыре торговых дня
  EXPECT_EQ(t.warmupBars().front().step, -4);
  EXPECT_EQ(t.warmupBars().front().close, Decimal::parse("100"));
}

TEST(TimelineWarmup, ZeroIsPlainLoad) {
  auto f = fixture1();
  const Timeline plain =
      Timeline::load(f.bars, f.calendar, f.snapshotId, f.instruments,
                     Timeframe::D1, 20240115, 20240122);
  const Timeline t = fromJan15(0);
  EXPECT_EQ(t.warmupSteps(), 0);
  EXPECT_TRUE(t.warmupBars().empty());
  ASSERT_EQ(t.totalSteps(), plain.totalSteps());
  for (std::int64_t s = 0; s < t.totalSteps(); ++s) {
    ASSERT_EQ(t.barsAt(s).size(), plain.barsAt(s).size());
    for (std::size_t i = 0; i < t.barsAt(s).size(); ++i)
      EXPECT_EQ(t.barsAt(s)[i], plain.barsAt(s)[i]);
  }
}

// Срез берёт W шагов прямо перед своим первым днём: из дней шкалы, а в начале -
// из её прогрева.
TEST(TimelineWarmup, SliceKeepsWarmupFromPrecedingSteps) {
  auto f = fixture1();
  // 11…22 января (8 дней), прогрев 2 шага - 9 и 10 января.
  const Timeline t =
      Timeline::loadWithWarmup(f.bars, f.calendar, f.snapshotId, f.instruments,
                               Timeframe::D1, 20240111, 20240122, 2);
  ASSERT_EQ(t.warmupSteps(), 2);

  const Timeline middle =
      t.slice(3, 5); // 15…17 января; прогрев - 11 и 12 января
  EXPECT_EQ(middle.warmupSteps(), 2);
  ASSERT_EQ(middle.warmupBars().size(), 3u); // у B нет бара 12-го
  EXPECT_EQ(middle.warmupBars()[0].step, -2);
  EXPECT_EQ(middle.warmupBars()[0].tradingDay, -1);
  EXPECT_EQ(middle.warmupBars()[0].close, Decimal::parse("103"));
  EXPECT_EQ(middle.warmupBars()[2].step, -1);
  EXPECT_EQ(middle.warmupBars()[2].tradingDay, 0);
  EXPECT_EQ(middle.warmupBars()[2].close, Decimal::parse("106"));

  const Timeline second =
      t.slice(2, 3); // прогрев - 10 января (прогрев шкалы) и 11 января (день 1)
  ASSERT_EQ(second.warmupSteps(), 2);
  EXPECT_EQ(second.warmupBars().front().close, Decimal::parse("101"));
  ASSERT_EQ(second.warmupBars().size(), 4u);
  EXPECT_EQ(second.warmupBars().back().instrument, 1); // B 11 января
  EXPECT_EQ(second.warmupBars().back().close, Decimal::parse("0.02"));
  EXPECT_EQ(second.warmupBars().back().step, -1);

  // Без прогрева срез, как и раньше, начинается без истории.
  const Timeline plain =
      Timeline::load(f.bars, f.calendar, f.snapshotId, f.instruments,
                     Timeframe::D1, 20240111, 20240122);
  EXPECT_EQ(plain.slice(3, 5).warmupSteps(), 0);
}

TEST(TimelineWarmup, WithWarmupChecksInput) {
  auto f = fixture1();
  const Timeline period =
      Timeline::load(f.bars, f.calendar, f.snapshotId, f.instruments,
                     Timeframe::D1, 20240115, 20240122);
  const Timeline before =
      Timeline::load(f.bars, f.calendar, f.snapshotId, f.instruments,
                     Timeframe::D1, 20240109, 20240112);
  EXPECT_THROW((void)period.withWarmup(before, -1), std::invalid_argument);
  // История, которая не раньше периода, - ошибка.
  EXPECT_THROW((void)period.withWarmup(period, 2), std::invalid_argument);
  // Другие бумаги - ошибка.
  const std::vector<Instrument> onlyA{f.instruments[0]};
  const Timeline otherInstruments =
      Timeline::load(f.bars, f.calendar, f.snapshotId, onlyA, Timeframe::D1,
                     20240109, 20240112);
  EXPECT_THROW((void)period.withWarmup(otherInstruments, 2),
               std::invalid_argument);
  // Повторный прогрев заменяет прежний.
  const Timeline twice = period.withWarmup(before, 3).withWarmup(before, 1);
  EXPECT_EQ(twice.warmupSteps(), 1);
  EXPECT_EQ(twice.warmupBars().size(), 1u); // 12 января - только A
}

} // namespace
} // namespace core
