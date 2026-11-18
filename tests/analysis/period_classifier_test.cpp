#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>
#include <vector>

#include "core/analysis/period_classifier.hpp"
#include "core/money/decimal.hpp"
#include "core/report/benchmark.hpp"

namespace {

core::DailyClose dc(std::int32_t Ymd, const char *Close) {
  return {Ymd, core::Decimal::parse(Close)};
}

} // namespace

TEST(PeriodClassifier, SyntheticSeriesKnownSigmaMddAndCategories) {
  // 9 торговых дней -> 6 окон длины L = 4.
  // Окно 0 (дни 0..3): 100, 100, 100, 100 -> sigma = 0, mdd = 0
  // Окно 1 (дни 1..4): 100, 100, 100, 101 -> небольшая волатильность
  // Окно 2 (дни 2..5): 100, 100, 101, 102
  // Окно 3 (дни 3..6): 100, 101, 102, 100
  // Окно 4 (дни 4..7): 101, 102, 100, 110
  // Окно 5 (дни 5..8): 102, 100, 110, 80  -> mdd = 1 - 80/110 = 3/11 (~0.2727
  // >= 0.20) -> Stress
  const std::vector<core::DailyClose> Series = {
      dc(20250101, "100"), dc(20250102, "100"), dc(20250103, "100"),
      dc(20250106, "100"), dc(20250107, "101"), dc(20250108, "102"),
      dc(20250109, "100"), dc(20250110, "110"), dc(20250113, "80"),
  };

  const core::PeriodClassification Res = core::classifyPeriods(Series, 4);
  ASSERT_EQ(Res.Windows.size(), 6u);

  // Порядок по дате начала (INV-4).
  for (std::size_t I = 1; I < Res.Windows.size(); ++I)
    EXPECT_LT(Res.Windows[I - 1].StartDate, Res.Windows[I].StartDate);

  // Окно 0: нулевые доходности -> sigma = 0, mdd = 0 -> Calm.
  EXPECT_EQ(Res.Windows[0].StartDate, 20250101);
  EXPECT_EQ(Res.Windows[0].EndDate, 20250106);
  EXPECT_DOUBLE_EQ(Res.Windows[0].Sigma, 0.0);
  EXPECT_DOUBLE_EQ(Res.Windows[0].Mdd, 0.0);
  EXPECT_EQ(Res.Windows[0].Category, core::PeriodCategory::Calm);

  // Окно 1: доходности 0, 0, ln(1.01).
  const double R = std::log(1.01);
  const double Mean = R / 3.0;
  const double Var =
      ((0.0 - Mean) * (0.0 - Mean) * 2.0 + (R - Mean) * (R - Mean)) / 2.0;
  const double ExpectedSigma1 = std::sqrt(Var) * std::sqrt(252.0);
  EXPECT_NEAR(Res.Windows[1].Sigma, ExpectedSigma1, 1e-12);
  EXPECT_DOUBLE_EQ(Res.Windows[1].Mdd, 0.0);

  // Окно 5: MDD = 1 - 80/110 = 3/11 >= 0.20 -> Stress.
  EXPECT_EQ(Res.Windows[5].StartDate, 20250108);
  EXPECT_EQ(Res.Windows[5].EndDate, 20250113);
  EXPECT_NEAR(Res.Windows[5].Mdd, 3.0 / 11.0, 1e-12);
  EXPECT_EQ(Res.Windows[5].Category, core::PeriodCategory::Stress);

  EXPECT_LE(Res.P33, Res.P80);
  // Окно 2 или 3 должно попасть в Medium (между P33 и P80, MDD < 10%).
  bool HasMedium = false;
  for (const auto &W : Res.Windows) {
    if (W.Category == core::PeriodCategory::Medium)
      HasMedium = true;
  }
  EXPECT_TRUE(HasMedium);
}

TEST(PeriodClassifier, SingleWindowVariantMatchesFullClassification) {
  const std::vector<core::DailyClose> Series = {
      dc(20250101, "100"), dc(20250102, "100"), dc(20250103, "100"),
      dc(20250106, "100"), dc(20250107, "101"), dc(20250108, "102"),
      dc(20250109, "100"), dc(20250110, "110"), dc(20250113, "80"),
  };

  const core::PeriodClassification All = core::classifyPeriods(Series, 4);
  const core::PeriodClassification Single =
      core::classifyPeriodAt(Series, 4, 20250108);
  ASSERT_EQ(Single.Windows.size(), 1u);
  EXPECT_DOUBLE_EQ(Single.P33, All.P33);
  EXPECT_DOUBLE_EQ(Single.P80, All.P80);
  EXPECT_EQ(Single.Windows[0].StartDate, All.Windows[5].StartDate);
  EXPECT_EQ(Single.Windows[0].EndDate, All.Windows[5].EndDate);
  EXPECT_DOUBLE_EQ(Single.Windows[0].Sigma, All.Windows[5].Sigma);
  EXPECT_DOUBLE_EQ(Single.Windows[0].Mdd, All.Windows[5].Mdd);
  EXPECT_EQ(Single.Windows[0].Category, All.Windows[5].Category);

  EXPECT_THROW(core::classifyPeriodAt(Series, 4, 20250110),
               std::invalid_argument);
}

TEST(PeriodClassifier, RejectsInvalidInputs) {
  const std::vector<core::DailyClose> Series = {
      dc(20250101, "100"),
      dc(20250102, "101"),
      dc(20250103, "102"),
  };
  EXPECT_THROW(core::classifyPeriods(Series, 2), std::invalid_argument);

  const std::vector<core::DailyClose> Unsorted = {
      dc(20250102, "100"),
      dc(20250101, "101"),
      dc(20250103, "102"),
  };
  EXPECT_THROW(core::classifyPeriods(Unsorted, 3), std::invalid_argument);
}
