#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/analysis/stress.hpp"
#include "core/market/timeline.hpp"
#include "support/fixture1.hpp"

namespace {

using core::applyStress;
using core::Bar;
using core::Decimal;
using core::Instrument;
using core::StepBar;
using core::StressScenario;
using core::Timeline;
using core::TradingDay;

constexpr std::int64_t kDayMs = 86'400'000;
constexpr std::int64_t kJan9Ms = 1'704'747'600'000; // 2024-01-09 00:00 МСК

struct Ohlcv {
  const char *o;
  const char *h;
  const char *l;
  const char *c;
  std::int64_t v;
};

Bar makeBar(std::int64_t day, const Ohlcv &x) {
  Bar b;
  b.tsOpenMs = kJan9Ms + day * kDayMs;
  b.availableAtMs = b.tsOpenMs + kDayMs;
  b.open = Decimal::parse(x.o);
  b.high = Decimal::parse(x.h);
  b.low = Decimal::parse(x.l);
  b.close = Decimal::parse(x.c);
  b.volume = x.v;
  return b;
}

// Одна бумага, шаг цены 0.01, лот 1, подряд идущие дни января (все торговые).
Timeline singleInstrument(const std::vector<Ohlcv> &series) {
  std::vector<Bar> bars;
  std::vector<TradingDay> calendar;
  for (std::size_t i = 0; i < series.size(); ++i) {
    bars.push_back(makeBar(static_cast<std::int64_t>(i), series[i]));
    calendar.push_back({20240109 + static_cast<std::int32_t>(i), true});
  }
  return Timeline::build({Instrument{"A", "A", 1, Decimal::parse("0.01")}},
                         {bars}, calendar);
}

void expectBar(const Timeline &t, std::int64_t step, const char *o,
               const char *h, const char *l, const char *c, std::int64_t lots) {
  const StepBar *b = t.barOf(0, step);
  ASSERT_NE(b, nullptr) << "шаг " << step;
  EXPECT_EQ(b->open.toString(2), o) << "open, шаг " << step;
  EXPECT_EQ(b->high.toString(2), h) << "high, шаг " << step;
  EXPECT_EQ(b->low.toString(2), l) << "low, шаг " << step;
  EXPECT_EQ(b->close.toString(2), c) << "close, шаг " << step;
  EXPECT_EQ(b->volumeLots, lots) << "объём, шаг " << step;
}

const std::vector<Ohlcv> kWorked = {{"100", "101", "99", "100", 1000},
                                    {"100", "103", "99", "102", 1000},
                                    {"102", "104", "101", "103", 1000},
                                    {"103", "103", "100", "101", 1000},
                                    {"101", "102", "100", "102", 1000}};

StressScenario scenario(std::int64_t startStep, const char *gap, const char *k,
                        std::int32_t n, const char *q) {
  return StressScenario{startStep, Decimal::parse(gap), Decimal::parse(k), n,
                        Decimal::parse(q)};
}

TEST(Stress, WorkedExample) {
  const Timeline base = singleInstrument(kWorked);
  const Timeline out = applyStress(base, scenario(2, "10", "3", 2, "0.2"));
  ASSERT_EQ(out.totalSteps(), 5);
  expectBar(out, 0, "100.00", "101.00", "99.00", "100.00", 1000);
  expectBar(out, 1, "100.00", "103.00", "99.00", "102.00", 1000);
  expectBar(out, 2, "71.40", "76.80", "68.70", "74.10", 200);
  expectBar(out, 3, "74.10", "74.10", "67.63", "69.78", 200);
  expectBar(out, 4, "69.78", "70.47", "69.09", "70.47", 1000);
  // f = c'/c = 69.78 / 90.9, 9 знаков HALF_UP
  EXPECT_EQ(Decimal::parse("69.78")
                .div(Decimal::parse("90.9"), core::RoundingMode::HALF_UP)
                .toString(),
            "0.767656766");
}

TEST(Stress, DoesNotMutateInputAndIsDeterministic) {
  const Timeline base = singleInstrument(kWorked);
  const Timeline copy = singleInstrument(kWorked);
  const StressScenario s = scenario(2, "10", "3", 2, "0.2");
  const Timeline a = applyStress(base, s);
  const Timeline b = applyStress(base, s);
  for (std::int64_t st = 0; st < base.totalSteps(); ++st) {
    EXPECT_EQ(*base.barOf(0, st), *copy.barOf(0, st))
        << "вход изменился, шаг " << st;
    EXPECT_EQ(*a.barOf(0, st), *b.barOf(0, st)) << "шаг " << st;
  }
}

TEST(Stress, NeutralScenarioKeepsData) {
  const Timeline base = singleInstrument(kWorked);
  const Timeline out = applyStress(base, scenario(2, "0", "1", 2, "1"));
  for (std::int64_t st = 0; st < base.totalSteps(); ++st) {
    EXPECT_EQ(*out.barOf(0, st), *base.barOf(0, st)) << "шаг " << st;
  }
}

TEST(Stress, GapOnlyWhenNoVolatilityWindow) {
  const Timeline out =
      applyStress(singleInstrument(kWorked), scenario(3, "10", "3", 0, "0.2"));
  expectBar(out, 2, "102.00", "104.00", "101.00", "103.00", 1000);
  expectBar(out, 3, "92.70", "92.70", "90.00", "90.90", 1000);
  expectBar(out, 4, "90.90", "91.80", "90.00", "91.80", 1000);
}

TEST(Stress, WindowLongerThanDataHasNoRecovery) {
  const Timeline out =
      applyStress(singleInstrument(kWorked), scenario(3, "10", "3", 20, "0.5"));
  const StepBar *last = out.barOf(0, 4);
  ASSERT_NE(last, nullptr);
  EXPECT_EQ(last->volumeLots, 500);
  EXPECT_LE(last->low, last->high);
}

TEST(Stress, StartBeyondEndChangesNothing) {
  const Timeline base = singleInstrument(kWorked);
  const Timeline out = applyStress(base, scenario(5, "10", "3", 2, "0.2"));
  for (std::int64_t st = 0; st < base.totalSteps(); ++st)
    EXPECT_EQ(*out.barOf(0, st), *base.barOf(0, st));
}

TEST(Stress, NoPreviousBarUsesOwnOpenAsReference) {
  // Опорное закрытие - open первого бара: гэп 10 % усиливается множителем k = 3
  // -> 100 * 0,7.
  const Timeline out =
      applyStress(singleInstrument({{"100", "100", "100", "100", 10}}),
                  scenario(0, "10", "3", 1, "1"));
  expectBar(out, 0, "70.00", "70.00", "70.00", "70.00", 10);
}

TEST(Stress, NonPositivePriceBecomesPriceStep) {
  // Гэп 50 %, k = 3: 100 * (1 + 3 * (0,5 - 1)) = -50 -> шаг цены.
  const Timeline out =
      applyStress(singleInstrument({{"100", "100", "100", "100", 10},
                                    {"100", "100", "100", "100", 10}}),
                  scenario(1, "50", "3", 1, "1"));
  expectBar(out, 1, "0.01", "0.01", "0.01", "0.01", 10);
}

TEST(Stress, VolumeFlooredToLots) {
  const Timeline out =
      applyStress(singleInstrument({{"10", "10", "10", "10", 9},
                                    {"10", "10", "10", "10", 4}}),
                  scenario(0, "0", "1", 2, "0.2"));
  EXPECT_EQ(out.barOf(0, 0)->volumeLots, 1); // floor(1,8)
  EXPECT_EQ(out.barOf(0, 1)->volumeLots, 0); // floor(0,8)
}

TEST(Stress, PricesAreRoundedToStepHalfUp) {
  // 100.05 * 0.9 = 90.045 -> 90.05, шаг 0.01.
  const Timeline out = applyStress(
      singleInstrument({{"100.05", "100.05", "100.05", "100.05", 1}}),
      scenario(0, "10", "1", 0, "1"));
  EXPECT_EQ(out.barOf(0, 0)->open.toString(2), "90.05");
}

TEST(Stress, EachInstrumentByItsOwnBars) {
  auto f = core::testing::fixture1();
  const Timeline base =
      Timeline::load(f.bars, f.calendar, f.snapshotId, f.instruments,
                     core::Timeframe::D1, 20240109, 20240122);
  // У B нет бара на шаге 3. t0 = 3: окно B - её первый бар на шаге >= 3 (шаг
  // 4), N = 1.
  const Timeline out = applyStress(base, scenario(3, "10", "1", 1, "0.5"));
  EXPECT_EQ(out.barOf(1, 3), nullptr);
  ASSERT_NE(out.barOf(1, 4), nullptr);
  EXPECT_EQ(out.barOf(1, 4)->volumeLots, base.barOf(1, 4)->volumeLots / 2);
  EXPECT_EQ(out.barOf(1, 5)->volumeLots, base.barOf(1, 5)->volumeLots);
  EXPECT_EQ(out.barOf(0, 3)->volumeLots, base.barOf(0, 3)->volumeLots / 2);
  EXPECT_EQ(*out.barOf(0, 2), *base.barOf(0, 2));
  EXPECT_EQ(*out.barOf(1, 2), *base.barOf(1, 2));
}

TEST(Stress, InvalidParametersThrow) {
  const Timeline base = singleInstrument(kWorked);
  EXPECT_THROW(applyStress(base, scenario(-1, "10", "3", 2, "0.2")),
               std::invalid_argument);
  EXPECT_THROW(applyStress(base, scenario(2, "100", "3", 2, "0.2")),
               std::invalid_argument);
  EXPECT_THROW(applyStress(base, scenario(2, "-1", "3", 2, "0.2")),
               std::invalid_argument);
  EXPECT_THROW(applyStress(base, scenario(2, "10", "-1", 2, "0.2")),
               std::invalid_argument);
  EXPECT_THROW(applyStress(base, scenario(2, "10", "3", -1, "0.2")),
               std::invalid_argument);
  EXPECT_THROW(applyStress(base, scenario(2, "10", "3", 2, "1.1")),
               std::invalid_argument);
  EXPECT_THROW(applyStress(base, scenario(2, "10", "3", 2, "-0.1")),
               std::invalid_argument);
}

} // namespace
