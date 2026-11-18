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

Bar bar(std::int64_t TsOpenMs, std::int64_t IntervalMs,
        std::int64_t Volume = 100, const char *Price = "10") {
  Bar B;
  B.TsOpenMs = TsOpenMs;
  B.AvailableAtMs = TsOpenMs + IntervalMs;
  B.Open = B.High = B.Low = B.Close = Decimal::parse(Price);
  B.Volume = Volume;
  return B;
}

Instrument instrument(const char *Key, std::int64_t LotSize = 1) {
  return Instrument{Key, Key, LotSize, Decimal::parse("0.01")};
}

Timeline fixtureTimeline() {
  auto F = core::testing::fixture1();
  return Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                        Timeframe::D1, 20240109, 20240122);
}

TEST(Timeframe, IntervalSeconds) {
  EXPECT_EQ(core::intervalSeconds(Timeframe::M1), 60);
  EXPECT_EQ(core::intervalSeconds(Timeframe::M5), 300);
  EXPECT_EQ(core::intervalSeconds(Timeframe::M10), 600);
  EXPECT_EQ(core::intervalSeconds(Timeframe::M15), 900);
  EXPECT_EQ(core::intervalSeconds(Timeframe::H1), 3600);
  EXPECT_EQ(core::intervalSeconds(Timeframe::D1), 86400);
}

TEST(Timeline, MergesInstrumentsByTime) {
  const Timeline T = fixtureTimeline();
  ASSERT_EQ(T.totalSteps(), 10);
  EXPECT_EQ(T.barOf(1, 3), nullptr);
  ASSERT_EQ(T.barsAt(3).size(), 1u);
  EXPECT_EQ(T.barsAt(3)[0].Instrument, 0);
  ASSERT_EQ(T.barsAt(2).size(), 2u);
  EXPECT_EQ(T.barsAt(2)[0].Instrument, 0);
  EXPECT_EQ(T.barsAt(2)[1].Instrument, 1);
  const core::StepBar *B = T.barOf(1, 4);
  ASSERT_NE(B, nullptr);
  EXPECT_TRUE(B->AfterHalt);
  EXPECT_EQ(B->Step, 4);
}

TEST(Timeline, FixtureValuesAndDays) {
  const Timeline T = fixtureTimeline();
  const core::StepBar *A0 = T.barOf(0, 0);
  ASSERT_NE(A0, nullptr);
  EXPECT_EQ(A0->Open, Decimal::parse("100"));
  EXPECT_EQ(A0->Close, Decimal::parse("100"));
  EXPECT_EQ(A0->High, Decimal::parse("101"));
  EXPECT_EQ(A0->Low, Decimal::parse("99"));
  EXPECT_EQ(A0->VolumeLots, 10'000);
  EXPECT_EQ(A0->TimeUnixMs, kJan9Ms);
  EXPECT_EQ(T.barOf(0, 3)->Close, Decimal::parse("106"));
  EXPECT_EQ(T.barOf(0, 4)->Open, Decimal::parse("106"));
  EXPECT_EQ(T.barOf(1, 0)->VolumeLots, 100'000);
  EXPECT_EQ(T.barOf(1, 0)->Close, Decimal::parse("0.02"));
  EXPECT_EQ(T.tradingDays(), 10);
  EXPECT_EQ(T.tradingDayOf(0), 1);
  EXPECT_EQ(T.tradingDayOf(9), 10);
  EXPECT_EQ(T.yyyymmddOf(0), 20240109);
  EXPECT_EQ(T.yyyymmddOf(3), 20240112);
  EXPECT_EQ(T.yyyymmddOf(4), 20240115);
  EXPECT_EQ(T.yyyymmddOf(9), 20240122);
  EXPECT_FALSE(T.barOf(0, 0)->AfterHalt);
}

TEST(Timeline, WeekendIsNotHalt) {
  const Timeline T = fixtureTimeline();
  EXPECT_FALSE(T.barOf(0, 3)->AfterHalt);
  EXPECT_FALSE(T.barOf(0, 4)->AfterHalt);
}

TEST(Timeline, MissingTradingDayIsHalt) {
  // 16 января бумага не торговалась, а день торговый: бар 17-го идёт после
  // перерыва.
  const std::vector<TradingDay> Calendar = {
      {20240115, true}, {20240116, true}, {20240117, true}};
  const Timeline T = Timeline::build(
      {instrument("A")},
      {{bar(kJan9Ms + 6 * kDayMs, kDayMs), bar(kJan9Ms + 8 * kDayMs, kDayMs)}},
      Calendar);
  ASSERT_EQ(T.totalSteps(), 2);
  EXPECT_FALSE(T.barOf(0, 0)->AfterHalt);
  EXPECT_TRUE(T.barOf(0, 1)->AfterHalt);
}

TEST(Timeline, NonTradingDayBetweenIsNotHalt) {
  const std::vector<TradingDay> Calendar = {
      {20240115, true}, {20240116, false}, {20240117, true}};
  const Timeline T = Timeline::build(
      {instrument("A")},
      {{bar(kJan9Ms + 6 * kDayMs, kDayMs), bar(kJan9Ms + 8 * kDayMs, kDayMs)}},
      Calendar);
  EXPECT_FALSE(T.barOf(0, 1)->AfterHalt);
}

TEST(Timeline, IntradayGap) {
  const std::int64_t Open = kJan9Ms + 10 * kHourMs; // 10:00 МСК
  const std::vector<TradingDay> Calendar = {{20240109, true}};
  const Timeline T =
      Timeline::build({instrument("A")},
                      {{bar(Open, kMinuteMs), bar(Open + kMinuteMs, kMinuteMs),
                        bar(Open + 21 * kMinuteMs, kMinuteMs),
                        bar(Open + 24 * kMinuteMs, kMinuteMs)}},
                      Calendar);
  ASSERT_EQ(T.totalSteps(), 4);
  EXPECT_FALSE(T.barOf(0, 1)->AfterHalt); // соседний бар
  EXPECT_TRUE(T.barOf(0, 2)->AfterHalt);  // разрыв 20 минут
  EXPECT_FALSE(T.barOf(0, 3)->AfterHalt); // разрыв 3 минуты
}

TEST(Timeline, IntradayGapBoundaries) {
  const std::int64_t Open = kJan9Ms + 10 * kHourMs;
  // M1: ровно 15 минут - перерыв, 14 минут - нет.
  const Timeline T = Timeline::build(
      {instrument("A")},
      {{bar(Open, kMinuteMs), bar(Open + 15 * kMinuteMs, kMinuteMs),
        bar(Open + 29 * kMinuteMs, kMinuteMs)}},
      {});
  EXPECT_TRUE(T.barOf(0, 1)->AfterHalt);
  EXPECT_FALSE(T.barOf(0, 2)->AfterHalt);
  // M10: разрыв 10 минут (1 интервал) - не перерыв, 20 минут - перерыв.
  constexpr std::int64_t kM10 = 10 * kMinuteMs;
  const Timeline T10 = Timeline::build(
      {instrument("A")},
      {{bar(Open, kM10), bar(Open + kM10, kM10), bar(Open + 3 * kM10, kM10)}},
      {});
  EXPECT_FALSE(T10.barOf(0, 1)->AfterHalt);
  EXPECT_TRUE(T10.barOf(0, 2)->AfterHalt);
  // H1: следующий час подряд - не перерыв, хотя больше 15 минут (меньше двух
  // интервалов).
  const Timeline Th =
      Timeline::build({instrument("A")},
                      {{bar(Open, kHourMs), bar(Open + kHourMs, kHourMs)}}, {});
  EXPECT_FALSE(Th.barOf(0, 1)->AfterHalt);
}

TEST(Timeline, HaltIsPerInstrument) {
  // B торгуется не на каждом шаге A: разрыв считается по барам самой бумаги.
  const std::int64_t Open = kJan9Ms + 10 * kHourMs;
  const Timeline T = Timeline::build(
      {instrument("A"), instrument("B")},
      {{bar(Open, kMinuteMs), bar(Open + kMinuteMs, kMinuteMs),
        bar(Open + 2 * kMinuteMs, kMinuteMs),
        bar(Open + 3 * kMinuteMs, kMinuteMs)},
       {bar(Open, kMinuteMs), bar(Open + 3 * kMinuteMs, kMinuteMs)}},
      {});
  ASSERT_EQ(T.totalSteps(), 4);
  EXPECT_EQ(T.barOf(1, 1), nullptr);
  EXPECT_FALSE(T.barOf(0, 3)->AfterHalt);
  EXPECT_FALSE(T.barOf(1, 3)->AfterHalt); // 3 минуты < 15
}

TEST(Timeline, VolumeInLots) {
  const Timeline T = Timeline::build({instrument("A", 10)},
                                     {{bar(kJan9Ms, kDayMs, 12345)}}, {});
  EXPECT_EQ(T.barOf(0, 0)->VolumeLots, 1234);
  const Timeline Small =
      Timeline::build({instrument("A", 10)}, {{bar(kJan9Ms, kDayMs, 9)}}, {});
  EXPECT_EQ(Small.barOf(0, 0)->VolumeLots, 0);
}

TEST(Timeline, MoscowDateAndTradingDays) {
  // Бар за минуту до полуночи МСК - дата 8 января; бар в полночь МСК - 9
  // января.
  const Timeline T = Timeline::build(
      {instrument("A")},
      {{bar(kJan9Ms - kMinuteMs, kMinuteMs), bar(kJan9Ms, kMinuteMs),
        bar(kJan9Ms + 3 * kMinuteMs, kMinuteMs),
        bar(kJan9Ms + kDayMs, kMinuteMs)}},
      {});
  ASSERT_EQ(T.totalSteps(), 4);
  EXPECT_EQ(T.yyyymmddOf(0), 20240108);
  EXPECT_EQ(T.yyyymmddOf(1), 20240109);
  EXPECT_EQ(T.tradingDayOf(0), 1);
  EXPECT_EQ(T.tradingDayOf(1), 2);
  EXPECT_EQ(T.tradingDayOf(2), 2);
  EXPECT_EQ(T.tradingDayOf(3), 3);
  EXPECT_EQ(T.tradingDays(), 3);
  EXPECT_TRUE(T.isLastStepOfDay(0));
  EXPECT_FALSE(T.isLastStepOfDay(1));
  EXPECT_TRUE(T.isLastStepOfDay(2));
  EXPECT_TRUE(T.isLastStepOfDay(3));
}

TEST(Timeline, MoscowDateAround2014OffsetTransition) {
  // До 2014-10-26 02:00 MSK действовало UTC+4 (с 2011-03-27 02:00 MSK),
  // после 2014-10-26 01:00 MSK - UTC+3; до 2011-03-27 - сезонный перевод.
  // 2014-10-24 (UTC+4): полночь МСК = 2014-10-23 20:00 UTC.
  const std::int64_t Oct24MidnightUtc4 = 16366 * kDayMs + 20 * 3600'000LL;
  // Wait: вычисляем от 2014-10-23 = 16366? Проверим через календарные дни.
  // 2014-10-23: 1970..2014 = 44 года, 11 високосных
  // (72,76,80,84,88,92,96,00,04,08,12)
  // -> 44*365 + 11 = 16071 (2014-01-01). + 31+28+31+30+31+30+31+31+30 + 22 =
  // 16071 + 295 = 16366!
  const std::int64_t Oct23BeforeMidnight =
      Oct24MidnightUtc4 - kMinuteMs; // 23:59 23.10 UTC+4
  const std::int64_t Oct26MidnightUtc4 =
      (16366 + 2) * kDayMs + 20 * 3600'000LL; // 2014-10-25 20:00 UTC
  const std::int64_t Oct26AfterSwitchUtc3 =
      (16366 + 2) * kDayMs +
      22 * 3600'000LL; // 2014-10-25 22:00 UTC = 01:00 26.10 UTC+3
  const std::int64_t Oct26LateUtc3 =
      (16366 + 3) * kDayMs + 20 * 3600'000LL +
      30 * kMinuteMs; // 2014-10-26 20:30 UTC = 23:30 26.10 UTC+3
  const std::int64_t Oct27MidnightUtc3 =
      (16366 + 3) * kDayMs +
      21 * 3600'000LL; // 2014-10-26 21:00 UTC = 00:00 27.10 UTC+3

  const Timeline T = Timeline::build(
      {instrument("A")},
      {{bar(Oct23BeforeMidnight, kMinuteMs), bar(Oct24MidnightUtc4, kMinuteMs),
        bar(Oct26MidnightUtc4, kMinuteMs), bar(Oct26AfterSwitchUtc3, kMinuteMs),
        bar(Oct26LateUtc3, kMinuteMs), bar(Oct27MidnightUtc3, kMinuteMs)}},
      {});
  ASSERT_EQ(T.totalSteps(), 6);
  EXPECT_EQ(T.yyyymmddOf(0), 20141023);
  EXPECT_EQ(T.yyyymmddOf(1), 20141024);
  EXPECT_EQ(T.yyyymmddOf(2), 20141026);
  EXPECT_EQ(T.yyyymmddOf(3), 20141026);
  EXPECT_EQ(T.yyyymmddOf(4), 20141026);
  EXPECT_EQ(T.yyyymmddOf(5), 20141027);

  // Проверка Timeline::load по границам суток вблизи перехода.
  core::InMemoryBarSource Bars;
  Bars.addSeries(
      "s", "A", 60,
      {bar(Oct23BeforeMidnight, kMinuteMs), bar(Oct24MidnightUtc4, kMinuteMs),
       bar(Oct26MidnightUtc4, kMinuteMs), bar(Oct26LateUtc3, kMinuteMs),
       bar(Oct27MidnightUtc3, kMinuteMs)});
  core::InMemoryCalendarSource Cal;
  Cal.add("s", {{20141024, true}, {20141025, false}, {20141026, false}});
  const Timeline Loaded = Timeline::load(Bars, Cal, "s", {instrument("A")},
                                         Timeframe::M1, 20141024, 20141026);
  ASSERT_EQ(Loaded.totalSteps(), 3);
  EXPECT_EQ(Loaded.yyyymmddOf(0), 20141024);
  EXPECT_EQ(Loaded.yyyymmddOf(1), 20141026);
  EXPECT_EQ(Loaded.yyyymmddOf(2), 20141026);
}

TEST(Timeline, LastBarOf) {
  const Timeline T = fixtureTimeline();
  ASSERT_NE(T.lastBarOf(1, 3), nullptr);
  EXPECT_EQ(T.lastBarOf(1, 3)->Step,
            2); // на шаге 3 у B бара нет - берётся прежний
  EXPECT_EQ(T.lastBarOf(1, 4)->Step, 4);
  EXPECT_EQ(T.lastBarOf(0, 0)->Step, 0);
  EXPECT_EQ(T.lastBarOf(0, -1), nullptr);
}

TEST(Timeline, LastBarOfBeforeFirstBar) {
  const Timeline T =
      Timeline::build({instrument("A"), instrument("B")},
                      {{bar(kJan9Ms, kDayMs), bar(kJan9Ms + kDayMs, kDayMs)},
                       {bar(kJan9Ms + kDayMs, kDayMs)}},
                      {});
  EXPECT_EQ(T.lastBarOf(1, 0), nullptr);
  EXPECT_NE(T.lastBarOf(1, 1), nullptr);
}

TEST(Timeline, IsLastStepOfDayDaily) {
  const Timeline T = fixtureTimeline();
  for (std::int64_t S = 0; S < 10; ++S)
    EXPECT_TRUE(T.isLastStepOfDay(S));
}

TEST(Timeline, SliceRenumbersSteps) {
  const Timeline T = fixtureTimeline();
  const Timeline S = T.slice(3, 5);
  ASSERT_EQ(S.totalSteps(), 3);
  EXPECT_EQ(S.tradingDayOf(0), 1);
  EXPECT_EQ(S.tradingDayOf(2), 3);
  EXPECT_EQ(S.tradingDays(), 3);
  EXPECT_EQ(S.barsAt(0)[0].Step, 0);
  EXPECT_EQ(S.barsAt(0)[0].TradingDay, 1);
  EXPECT_EQ(S.yyyymmddOf(0), 20240111);
  EXPECT_EQ(S.barOf(0, 0)->Close, Decimal::parse("103"));
  EXPECT_EQ(S.barOf(1, 1), nullptr); // 12 января: у B бара нет
  EXPECT_EQ(S.barOf(1, 2)->Step, 2);
  EXPECT_TRUE(S.barOf(1, 2)->AfterHalt);
  ASSERT_NE(S.lastBarOf(1, 1), nullptr);
  EXPECT_EQ(S.lastBarOf(1, 1)->Step, 0);
  EXPECT_EQ(S.instruments().size(), 2u);
  EXPECT_EQ(T.totalSteps(), 10); // исходная шкала не изменилась
}

TEST(Timeline, SliceBounds) {
  const Timeline T = fixtureTimeline();
  EXPECT_EQ(T.slice(1, 10).totalSteps(), 10);
  EXPECT_THROW(T.slice(0, 3), std::invalid_argument);
  EXPECT_THROW(T.slice(5, 11), std::invalid_argument);
  EXPECT_THROW(T.slice(6, 5), std::invalid_argument);
}

TEST(Timeline, LoadUsesSources) {
  auto F = core::testing::fixture1();
  const Timeline T =
      Timeline::load(F.Bars, F.Calendar, "fixture-1", F.Instruments,
                     Timeframe::D1, 20240109, 20240122);
  EXPECT_EQ(T.totalSteps(), 10);
  const Timeline Part =
      Timeline::load(F.Bars, F.Calendar, "fixture-1", F.Instruments,
                     Timeframe::D1, 20240110, 20240112);
  EXPECT_EQ(Part.totalSteps(), 3);
  EXPECT_EQ(Part.yyyymmddOf(0), 20240110);
  EXPECT_EQ(Part.yyyymmddOf(2), 20240112);
  EXPECT_THROW(Timeline::load(F.Bars, F.Calendar, "no-such", F.Instruments,
                              Timeframe::D1, 20240109, 20240122),
               core::SnapshotNotFound);
}

TEST(Timeline, LoadFirstBarOfRangeIsNotAfterHalt) {
  auto F = core::testing::fixture1();
  const Timeline T =
      Timeline::load(F.Bars, F.Calendar, "fixture-1", F.Instruments,
                     Timeframe::D1, 20240115, 20240119);
  EXPECT_FALSE(T.barOf(0, 0)->AfterHalt);
  EXPECT_FALSE(T.barOf(1, 0)->AfterHalt);
}

TEST(Timeline, RangeChecks) {
  const Timeline T = fixtureTimeline();
  EXPECT_THROW(T.barsAt(10), std::out_of_range);
  EXPECT_THROW(T.barsAt(-1), std::out_of_range);
  EXPECT_THROW(T.barOf(0, 10), std::out_of_range);
  EXPECT_THROW(T.barOf(2, 0), std::out_of_range);
  EXPECT_THROW(T.tradingDayOf(10), std::out_of_range);
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
  const Timeline T = Timeline::build({instrument("A")}, {{}}, {});
  EXPECT_EQ(T.totalSteps(), 0);
  EXPECT_EQ(T.tradingDays(), 0);
}

} // namespace
