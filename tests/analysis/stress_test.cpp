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
  const char *O;
  const char *H;
  const char *L;
  const char *C;
  std::int64_t V;
};

Bar makeBar(std::int64_t Day, const Ohlcv &X) {
  Bar B;
  B.TsOpenMs = kJan9Ms + Day * kDayMs;
  B.AvailableAtMs = B.TsOpenMs + kDayMs;
  B.Open = Decimal::parse(X.O);
  B.High = Decimal::parse(X.H);
  B.Low = Decimal::parse(X.L);
  B.Close = Decimal::parse(X.C);
  B.Volume = X.V;
  return B;
}

// Одна бумага, шаг цены 0.01, лот 1, подряд идущие дни января (все торговые).
Timeline singleInstrument(const std::vector<Ohlcv> &Series) {
  std::vector<Bar> Bars;
  std::vector<TradingDay> Calendar;
  for (std::size_t I = 0; I < Series.size(); ++I) {
    Bars.push_back(makeBar(static_cast<std::int64_t>(I), Series[I]));
    Calendar.push_back({20240109 + static_cast<std::int32_t>(I), true});
  }
  return Timeline::build({Instrument{"A", "A", 1, Decimal::parse("0.01")}},
                         {Bars}, Calendar);
}

void expectBar(const Timeline &T, std::int64_t Step, const char *O,
               const char *H, const char *L, const char *C, std::int64_t Lots) {
  const StepBar *B = T.barOf(0, Step);
  ASSERT_NE(B, nullptr) << "шаг " << Step;
  EXPECT_EQ(B->Open.toString(2), O) << "open, шаг " << Step;
  EXPECT_EQ(B->High.toString(2), H) << "high, шаг " << Step;
  EXPECT_EQ(B->Low.toString(2), L) << "low, шаг " << Step;
  EXPECT_EQ(B->Close.toString(2), C) << "close, шаг " << Step;
  EXPECT_EQ(B->VolumeLots, Lots) << "объём, шаг " << Step;
}

const std::vector<Ohlcv> kWorked = {{"100", "101", "99", "100", 1000},
                                    {"100", "103", "99", "102", 1000},
                                    {"102", "104", "101", "103", 1000},
                                    {"103", "103", "100", "101", 1000},
                                    {"101", "102", "100", "102", 1000}};

StressScenario scenario(std::int64_t StartStep, const char *Gap, const char *K,
                        std::int32_t N, const char *Q) {
  return StressScenario{StartStep, Decimal::parse(Gap), Decimal::parse(K), N,
                        Decimal::parse(Q)};
}

TEST(Stress, WorkedExample) {
  const Timeline Base = singleInstrument(kWorked);
  const Timeline Out = applyStress(Base, scenario(2, "10", "3", 2, "0.2"));
  ASSERT_EQ(Out.totalSteps(), 5);
  expectBar(Out, 0, "100.00", "101.00", "99.00", "100.00", 1000);
  expectBar(Out, 1, "100.00", "103.00", "99.00", "102.00", 1000);
  expectBar(Out, 2, "71.40", "76.80", "68.70", "74.10", 200);
  expectBar(Out, 3, "74.10", "74.10", "67.63", "69.78", 200);
  expectBar(Out, 4, "69.78", "70.47", "69.09", "70.47", 1000);
  // f = c'/c = 69.78 / 90.9, 9 знаков HALF_UP
  EXPECT_EQ(Decimal::parse("69.78")
                .div(Decimal::parse("90.9"), core::RoundingMode::HalfUp)
                .toString(),
            "0.767656766");
}

TEST(Stress, DoesNotMutateInputAndIsDeterministic) {
  const Timeline Base = singleInstrument(kWorked);
  const Timeline Copy = singleInstrument(kWorked);
  const StressScenario S = scenario(2, "10", "3", 2, "0.2");
  const Timeline A = applyStress(Base, S);
  const Timeline B = applyStress(Base, S);
  for (std::int64_t St = 0; St < Base.totalSteps(); ++St) {
    EXPECT_EQ(*Base.barOf(0, St), *Copy.barOf(0, St))
        << "вход изменился, шаг " << St;
    EXPECT_EQ(*A.barOf(0, St), *B.barOf(0, St)) << "шаг " << St;
  }
}

TEST(Stress, NeutralScenarioKeepsData) {
  const Timeline Base = singleInstrument(kWorked);
  const Timeline Out = applyStress(Base, scenario(2, "0", "1", 2, "1"));
  for (std::int64_t St = 0; St < Base.totalSteps(); ++St) {
    EXPECT_EQ(*Out.barOf(0, St), *Base.barOf(0, St)) << "шаг " << St;
  }
}

TEST(Stress, GapOnlyWhenNoVolatilityWindow) {
  const Timeline Out =
      applyStress(singleInstrument(kWorked), scenario(3, "10", "3", 0, "0.2"));
  expectBar(Out, 2, "102.00", "104.00", "101.00", "103.00", 1000);
  expectBar(Out, 3, "92.70", "92.70", "90.00", "90.90", 1000);
  expectBar(Out, 4, "90.90", "91.80", "90.00", "91.80", 1000);
}

TEST(Stress, WindowLongerThanDataHasNoRecovery) {
  const Timeline Out =
      applyStress(singleInstrument(kWorked), scenario(3, "10", "3", 20, "0.5"));
  const StepBar *Last = Out.barOf(0, 4);
  ASSERT_NE(Last, nullptr);
  EXPECT_EQ(Last->VolumeLots, 500);
  EXPECT_LE(Last->Low, Last->High);
}

TEST(Stress, StartBeyondEndChangesNothing) {
  const Timeline Base = singleInstrument(kWorked);
  const Timeline Out = applyStress(Base, scenario(5, "10", "3", 2, "0.2"));
  for (std::int64_t St = 0; St < Base.totalSteps(); ++St)
    EXPECT_EQ(*Out.barOf(0, St), *Base.barOf(0, St));
}

TEST(Stress, NoPreviousBarUsesOwnOpenAsReference) {
  // Опорное закрытие - open первого бара: гэп 10 % усиливается множителем k = 3
  // -> 100 * 0,7.
  const Timeline Out =
      applyStress(singleInstrument({{"100", "100", "100", "100", 10}}),
                  scenario(0, "10", "3", 1, "1"));
  expectBar(Out, 0, "70.00", "70.00", "70.00", "70.00", 10);
}

TEST(Stress, NonPositivePriceBecomesPriceStep) {
  // Гэп 50 %, k = 3: 100 * (1 + 3 * (0,5 - 1)) = -50 -> шаг цены.
  const Timeline Out =
      applyStress(singleInstrument({{"100", "100", "100", "100", 10},
                                    {"100", "100", "100", "100", 10}}),
                  scenario(1, "50", "3", 1, "1"));
  expectBar(Out, 1, "0.01", "0.01", "0.01", "0.01", 10);
}

TEST(Stress, VolumeFlooredToLots) {
  const Timeline Out =
      applyStress(singleInstrument({{"10", "10", "10", "10", 9},
                                    {"10", "10", "10", "10", 4}}),
                  scenario(0, "0", "1", 2, "0.2"));
  EXPECT_EQ(Out.barOf(0, 0)->VolumeLots, 1); // floor(1,8)
  EXPECT_EQ(Out.barOf(0, 1)->VolumeLots, 0); // floor(0,8)
}

TEST(Stress, PricesAreRoundedToStepHalfUp) {
  // 100.05 * 0.9 = 90.045 -> 90.05, шаг 0.01.
  const Timeline Out = applyStress(
      singleInstrument({{"100.05", "100.05", "100.05", "100.05", 1}}),
      scenario(0, "10", "1", 0, "1"));
  EXPECT_EQ(Out.barOf(0, 0)->Open.toString(2), "90.05");
}

TEST(Stress, EachInstrumentByItsOwnBars) {
  auto F = core::testing::fixture1();
  const Timeline Base =
      Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                     core::Timeframe::D1, 20240109, 20240122);
  // У B нет бара на шаге 3. t0 = 3: окно B - её первый бар на шаге >= 3 (шаг
  // 4), N = 1.
  const Timeline Out = applyStress(Base, scenario(3, "10", "1", 1, "0.5"));
  EXPECT_EQ(Out.barOf(1, 3), nullptr);
  ASSERT_NE(Out.barOf(1, 4), nullptr);
  EXPECT_EQ(Out.barOf(1, 4)->VolumeLots, Base.barOf(1, 4)->VolumeLots / 2);
  EXPECT_EQ(Out.barOf(1, 5)->VolumeLots, Base.barOf(1, 5)->VolumeLots);
  EXPECT_EQ(Out.barOf(0, 3)->VolumeLots, Base.barOf(0, 3)->VolumeLots / 2);
  EXPECT_EQ(*Out.barOf(0, 2), *Base.barOf(0, 2));
  EXPECT_EQ(*Out.barOf(1, 2), *Base.barOf(1, 2));
}

TEST(Stress, InvalidParametersThrow) {
  const Timeline Base = singleInstrument(kWorked);
  EXPECT_THROW(applyStress(Base, scenario(-1, "10", "3", 2, "0.2")),
               std::invalid_argument);
  EXPECT_THROW(applyStress(Base, scenario(2, "100", "3", 2, "0.2")),
               std::invalid_argument);
  EXPECT_THROW(applyStress(Base, scenario(2, "-1", "3", 2, "0.2")),
               std::invalid_argument);
  EXPECT_THROW(applyStress(Base, scenario(2, "10", "-1", 2, "0.2")),
               std::invalid_argument);
  EXPECT_THROW(applyStress(Base, scenario(2, "10", "3", -1, "0.2")),
               std::invalid_argument);
  EXPECT_THROW(applyStress(Base, scenario(2, "10", "3", 2, "1.1")),
               std::invalid_argument);
  EXPECT_THROW(applyStress(Base, scenario(2, "10", "3", 2, "-0.1")),
               std::invalid_argument);
}

} // namespace
