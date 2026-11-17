#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/market/timeline.hpp"
#include "support/fixture1.hpp"

namespace {

using core::Bar;
using core::Decimal;
using core::Instrument;
using core::Timeframe;
using core::Timeline;
using core::TradingDay;

constexpr std::int64_t kMinuteMs = 60'000;
constexpr std::int64_t kHourMs = 3'600'000;
constexpr std::int64_t kDayMs = 86'400'000;
// 2024-01-09 00:00 МСК = 2024-01-08 21:00 UTC.
constexpr std::int64_t kJan9Ms = 1'704'747'600'000;

Bar bar(std::int64_t tsOpenMs, std::int64_t intervalMs,
        std::int64_t volume = 100, const char *price = "10") {
  Bar b;
  b.tsOpenMs = tsOpenMs;
  b.availableAtMs = tsOpenMs + intervalMs;
  b.open = b.high = b.low = b.close = Decimal::parse(price);
  b.volume = volume;
  return b;
}

Instrument instrument(const char *key, std::int64_t lotSize = 1) {
  return Instrument{key, key, lotSize, Decimal::parse("0.01")};
}

Timeline fixtureTimeline() {
  auto f = core::testing::fixture1();
  return Timeline::load(f.bars, f.calendar, f.snapshotId, f.instruments,
                        Timeframe::D1, 20240109, 20240122);
}

TEST(Timeframe, IntervalSeconds) {
  EXPECT_EQ(core::intervalSeconds(Timeframe::S1), 1);
  EXPECT_EQ(core::intervalSeconds(Timeframe::S10), 10);
  EXPECT_EQ(core::intervalSeconds(Timeframe::M1), 60);
  EXPECT_EQ(core::intervalSeconds(Timeframe::M5), 300);
  EXPECT_EQ(core::intervalSeconds(Timeframe::M10), 600);
  EXPECT_EQ(core::intervalSeconds(Timeframe::M15), 900);
  EXPECT_EQ(core::intervalSeconds(Timeframe::H1), 3600);
  EXPECT_EQ(core::intervalSeconds(Timeframe::D1), 86400);
}

TEST(Timeline, MergesInstrumentsByTime) {
  const Timeline t = fixtureTimeline();
  ASSERT_EQ(t.totalSteps(), 10);
  EXPECT_EQ(t.barOf(1, 3), nullptr);
  ASSERT_EQ(t.barsAt(3).size(), 1u);
  EXPECT_EQ(t.barsAt(3)[0].instrument, 0);
  ASSERT_EQ(t.barsAt(2).size(), 2u);
  EXPECT_EQ(t.barsAt(2)[0].instrument, 0);
  EXPECT_EQ(t.barsAt(2)[1].instrument, 1);
  const core::StepBar *b = t.barOf(1, 4);
  ASSERT_NE(b, nullptr);
  EXPECT_TRUE(b->afterHalt);
  EXPECT_EQ(b->step, 4);
}

TEST(Timeline, FixtureValuesAndDays) {
  const Timeline t = fixtureTimeline();
  const core::StepBar *a0 = t.barOf(0, 0);
  ASSERT_NE(a0, nullptr);
  EXPECT_EQ(a0->open, Decimal::parse("100"));
  EXPECT_EQ(a0->close, Decimal::parse("100"));
  EXPECT_EQ(a0->high, Decimal::parse("101"));
  EXPECT_EQ(a0->low, Decimal::parse("99"));
  EXPECT_EQ(a0->volumeLots, 10'000);
  EXPECT_EQ(a0->timeUnixMs, kJan9Ms);
  EXPECT_EQ(t.barOf(0, 3)->close, Decimal::parse("106"));
  EXPECT_EQ(t.barOf(0, 4)->open, Decimal::parse("106"));
  EXPECT_EQ(t.barOf(1, 0)->volumeLots, 100'000);
  EXPECT_EQ(t.barOf(1, 0)->close, Decimal::parse("0.02"));
  EXPECT_EQ(t.tradingDays(), 10);
  EXPECT_EQ(t.tradingDayOf(0), 1);
  EXPECT_EQ(t.tradingDayOf(9), 10);
  EXPECT_EQ(t.yyyymmddOf(0), 20240109);
  EXPECT_EQ(t.yyyymmddOf(3), 20240112);
  EXPECT_EQ(t.yyyymmddOf(4), 20240115);
  EXPECT_EQ(t.yyyymmddOf(9), 20240122);
  EXPECT_FALSE(t.barOf(0, 0)->afterHalt);
}

TEST(Timeline, WeekendIsNotHalt) {
  const Timeline t = fixtureTimeline();
  EXPECT_FALSE(t.barOf(0, 3)->afterHalt);
  EXPECT_FALSE(t.barOf(0, 4)->afterHalt);
}

TEST(Timeline, MissingTradingDayIsHalt) {
  // 16 января бумага не торговалась, а день торговый: бар 17-го идёт после
  // перерыва.
  const std::vector<TradingDay> calendar = {
      {20240115, true}, {20240116, true}, {20240117, true}};
  const Timeline t = Timeline::build(
      {instrument("A")},
      {{bar(kJan9Ms + 6 * kDayMs, kDayMs), bar(kJan9Ms + 8 * kDayMs, kDayMs)}},
      calendar);
  ASSERT_EQ(t.totalSteps(), 2);
  EXPECT_FALSE(t.barOf(0, 0)->afterHalt);
  EXPECT_TRUE(t.barOf(0, 1)->afterHalt);
}

TEST(Timeline, NonTradingDayBetweenIsNotHalt) {
  const std::vector<TradingDay> calendar = {
      {20240115, true}, {20240116, false}, {20240117, true}};
  const Timeline t = Timeline::build(
      {instrument("A")},
      {{bar(kJan9Ms + 6 * kDayMs, kDayMs), bar(kJan9Ms + 8 * kDayMs, kDayMs)}},
      calendar);
  EXPECT_FALSE(t.barOf(0, 1)->afterHalt);
}

TEST(Timeline, IntradayGap) {
  const std::int64_t open = kJan9Ms + 10 * kHourMs; // 10:00 МСК
  const std::vector<TradingDay> calendar = {{20240109, true}};
  const Timeline t =
      Timeline::build({instrument("A")},
                      {{bar(open, kMinuteMs), bar(open + kMinuteMs, kMinuteMs),
                        bar(open + 21 * kMinuteMs, kMinuteMs),
                        bar(open + 24 * kMinuteMs, kMinuteMs)}},
                      calendar);
  ASSERT_EQ(t.totalSteps(), 4);
  EXPECT_FALSE(t.barOf(0, 1)->afterHalt); // соседний бар
  EXPECT_TRUE(t.barOf(0, 2)->afterHalt);  // разрыв 20 минут
  EXPECT_FALSE(t.barOf(0, 3)->afterHalt); // разрыв 3 минуты
}

TEST(Timeline, IntradayGapBoundaries) {
  const std::int64_t open = kJan9Ms + 10 * kHourMs;
  // M1: ровно 15 минут - перерыв, 14 минут - нет.
  const Timeline t = Timeline::build(
      {instrument("A")},
      {{bar(open, kMinuteMs), bar(open + 15 * kMinuteMs, kMinuteMs),
        bar(open + 29 * kMinuteMs, kMinuteMs)}},
      {});
  EXPECT_TRUE(t.barOf(0, 1)->afterHalt);
  EXPECT_FALSE(t.barOf(0, 2)->afterHalt);
  // M10: разрыв 10 минут (1 интервал) - не перерыв, 20 минут - перерыв.
  constexpr std::int64_t kM10 = 10 * kMinuteMs;
  const Timeline t10 = Timeline::build(
      {instrument("A")},
      {{bar(open, kM10), bar(open + kM10, kM10), bar(open + 3 * kM10, kM10)}},
      {});
  EXPECT_FALSE(t10.barOf(0, 1)->afterHalt);
  EXPECT_TRUE(t10.barOf(0, 2)->afterHalt);
  // H1: следующий час подряд - не перерыв, хотя больше 15 минут (меньше двух
  // интервалов).
  const Timeline th =
      Timeline::build({instrument("A")},
                      {{bar(open, kHourMs), bar(open + kHourMs, kHourMs)}}, {});
  EXPECT_FALSE(th.barOf(0, 1)->afterHalt);
}

TEST(Timeline, HaltIsPerInstrument) {
  // B торгуется не на каждом шаге A: разрыв считается по барам самой бумаги.
  const std::int64_t open = kJan9Ms + 10 * kHourMs;
  const Timeline t = Timeline::build(
      {instrument("A"), instrument("B")},
      {{bar(open, kMinuteMs), bar(open + kMinuteMs, kMinuteMs),
        bar(open + 2 * kMinuteMs, kMinuteMs),
        bar(open + 3 * kMinuteMs, kMinuteMs)},
       {bar(open, kMinuteMs), bar(open + 3 * kMinuteMs, kMinuteMs)}},
      {});
  ASSERT_EQ(t.totalSteps(), 4);
  EXPECT_EQ(t.barOf(1, 1), nullptr);
  EXPECT_FALSE(t.barOf(0, 3)->afterHalt);
  EXPECT_FALSE(t.barOf(1, 3)->afterHalt); // 3 минуты < 15
}

TEST(Timeline, VolumeInLots) {
  const Timeline t = Timeline::build({instrument("A", 10)},
                                     {{bar(kJan9Ms, kDayMs, 12345)}}, {});
  EXPECT_EQ(t.barOf(0, 0)->volumeLots, 1234);
  const Timeline small =
      Timeline::build({instrument("A", 10)}, {{bar(kJan9Ms, kDayMs, 9)}}, {});
  EXPECT_EQ(small.barOf(0, 0)->volumeLots, 0);
}

TEST(Timeline, MoscowDateAndTradingDays) {
  // Бар за минуту до полуночи МСК - дата 8 января; бар в полночь МСК - 9
  // января.
  const Timeline t = Timeline::build(
      {instrument("A")},
      {{bar(kJan9Ms - kMinuteMs, kMinuteMs), bar(kJan9Ms, kMinuteMs),
        bar(kJan9Ms + 3 * kMinuteMs, kMinuteMs),
        bar(kJan9Ms + kDayMs, kMinuteMs)}},
      {});
  ASSERT_EQ(t.totalSteps(), 4);
  EXPECT_EQ(t.yyyymmddOf(0), 20240108);
  EXPECT_EQ(t.yyyymmddOf(1), 20240109);
  EXPECT_EQ(t.tradingDayOf(0), 1);
  EXPECT_EQ(t.tradingDayOf(1), 2);
  EXPECT_EQ(t.tradingDayOf(2), 2);
  EXPECT_EQ(t.tradingDayOf(3), 3);
  EXPECT_EQ(t.tradingDays(), 3);
  EXPECT_TRUE(t.isLastStepOfDay(0));
  EXPECT_FALSE(t.isLastStepOfDay(1));
  EXPECT_TRUE(t.isLastStepOfDay(2));
  EXPECT_TRUE(t.isLastStepOfDay(3));
}

TEST(Timeline, LastBarOf) {
  const Timeline t = fixtureTimeline();
  ASSERT_NE(t.lastBarOf(1, 3), nullptr);
  EXPECT_EQ(t.lastBarOf(1, 3)->step,
            2); // на шаге 3 у B бара нет - берётся прежний
  EXPECT_EQ(t.lastBarOf(1, 4)->step, 4);
  EXPECT_EQ(t.lastBarOf(0, 0)->step, 0);
  EXPECT_EQ(t.lastBarOf(0, -1), nullptr);
}

TEST(Timeline, LastBarOfBeforeFirstBar) {
  const Timeline t =
      Timeline::build({instrument("A"), instrument("B")},
                      {{bar(kJan9Ms, kDayMs), bar(kJan9Ms + kDayMs, kDayMs)},
                       {bar(kJan9Ms + kDayMs, kDayMs)}},
                      {});
  EXPECT_EQ(t.lastBarOf(1, 0), nullptr);
  EXPECT_NE(t.lastBarOf(1, 1), nullptr);
}

TEST(Timeline, IsLastStepOfDayDaily) {
  const Timeline t = fixtureTimeline();
  for (std::int64_t s = 0; s < 10; ++s)
    EXPECT_TRUE(t.isLastStepOfDay(s));
}

TEST(Timeline, SliceRenumbersSteps) {
  const Timeline t = fixtureTimeline();
  const Timeline s = t.slice(3, 5);
  ASSERT_EQ(s.totalSteps(), 3);
  EXPECT_EQ(s.tradingDayOf(0), 1);
  EXPECT_EQ(s.tradingDayOf(2), 3);
  EXPECT_EQ(s.tradingDays(), 3);
  EXPECT_EQ(s.barsAt(0)[0].step, 0);
  EXPECT_EQ(s.barsAt(0)[0].tradingDay, 1);
  EXPECT_EQ(s.yyyymmddOf(0), 20240111);
  EXPECT_EQ(s.barOf(0, 0)->close, Decimal::parse("103"));
  EXPECT_EQ(s.barOf(1, 1), nullptr); // 12 января: у B бара нет
  EXPECT_EQ(s.barOf(1, 2)->step, 2);
  EXPECT_TRUE(s.barOf(1, 2)->afterHalt);
  ASSERT_NE(s.lastBarOf(1, 1), nullptr);
  EXPECT_EQ(s.lastBarOf(1, 1)->step, 0);
  EXPECT_EQ(s.instruments().size(), 2u);
  EXPECT_EQ(t.totalSteps(), 10); // исходная шкала не изменилась
}

TEST(Timeline, SliceBounds) {
  const Timeline t = fixtureTimeline();
  EXPECT_EQ(t.slice(1, 10).totalSteps(), 10);
  EXPECT_THROW(t.slice(0, 3), std::invalid_argument);
  EXPECT_THROW(t.slice(5, 11), std::invalid_argument);
  EXPECT_THROW(t.slice(6, 5), std::invalid_argument);
}

TEST(Timeline, LoadUsesSources) {
  auto f = core::testing::fixture1();
  const Timeline t =
      Timeline::load(f.bars, f.calendar, "fixture-1", f.instruments,
                     Timeframe::D1, 20240109, 20240122);
  EXPECT_EQ(t.totalSteps(), 10);
  const Timeline part =
      Timeline::load(f.bars, f.calendar, "fixture-1", f.instruments,
                     Timeframe::D1, 20240110, 20240112);
  EXPECT_EQ(part.totalSteps(), 3);
  EXPECT_EQ(part.yyyymmddOf(0), 20240110);
  EXPECT_EQ(part.yyyymmddOf(2), 20240112);
  EXPECT_THROW(Timeline::load(f.bars, f.calendar, "no-such", f.instruments,
                              Timeframe::D1, 20240109, 20240122),
               core::SnapshotNotFound);
}

TEST(Timeline, LoadFirstBarOfRangeIsNotAfterHalt) {
  auto f = core::testing::fixture1();
  const Timeline t =
      Timeline::load(f.bars, f.calendar, "fixture-1", f.instruments,
                     Timeframe::D1, 20240115, 20240119);
  EXPECT_FALSE(t.barOf(0, 0)->afterHalt);
  EXPECT_FALSE(t.barOf(1, 0)->afterHalt);
}

TEST(Timeline, MutableBar) {
  Timeline t = fixtureTimeline();
  t.mutableBar(2, 0).close = Decimal::parse("1");
  EXPECT_EQ(t.barOf(0, 2)->close, Decimal::parse("1"));
  EXPECT_EQ(t.lastBarOf(0, 5)->step, 5);
  EXPECT_THROW(t.mutableBar(3, 1), std::out_of_range);
}

TEST(Timeline, RangeChecks) {
  const Timeline t = fixtureTimeline();
  EXPECT_THROW(t.barsAt(10), std::out_of_range);
  EXPECT_THROW(t.barsAt(-1), std::out_of_range);
  EXPECT_THROW(t.barOf(0, 10), std::out_of_range);
  EXPECT_THROW(t.barOf(2, 0), std::out_of_range);
  EXPECT_THROW(t.tradingDayOf(10), std::out_of_range);
}

TEST(Timeline, RejectsBadInput) {
  EXPECT_THROW(Timeline::build({instrument("A")}, {}, {}),
               std::invalid_argument);
  EXPECT_THROW(Timeline::build(
                   {instrument("A")},
                   {{bar(kJan9Ms + kDayMs, kDayMs), bar(kJan9Ms, kDayMs)}}, {}),
               std::invalid_argument);
  EXPECT_THROW(Timeline::build({instrument("A", 0)}, {{}}, {}),
               std::invalid_argument);
}

TEST(Timeline, EmptyTimeline) {
  const Timeline t = Timeline::build({instrument("A")}, {{}}, {});
  EXPECT_EQ(t.totalSteps(), 0);
  EXPECT_EQ(t.tradingDays(), 0);
}

} // namespace
