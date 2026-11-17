#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "core/dsl/indicators.hpp"

using core::dsl::Indicator;
using core::dsl::makeIndicator;
using core::dsl::Value;

namespace {

constexpr double kEps = 1e-9;
constexpr Value kNa = std::nullopt;

const std::vector<double> kClose = {10, 11, 12, 11, 13, 14, 13, 15, 16, 15};

std::vector<double> shifted(double Delta) {
  std::vector<double> Out;
  for (double C : kClose)
    Out.push_back(C + Delta);
  return Out;
}

void expectSeries(const std::vector<Value> &Actual,
                  const std::vector<Value> &Expected) {
  ASSERT_EQ(Actual.size(), Expected.size());
  for (std::size_t I = 0; I < Expected.size(); ++I) {
    ASSERT_EQ(Actual[I].has_value(), Expected[I].has_value()) << "бар " << I;
    if (Expected[I]) {
      EXPECT_NEAR(*Actual[I], *Expected[I], kEps) << "бар " << I;
    }
  }
}

std::vector<Value> runSingle(const char *Name, std::int64_t N,
                             const std::vector<double> &X) {
  auto Ind = makeIndicator(Name, {N});
  std::vector<Value> Out;
  for (double V : X)
    Out.push_back(Ind->update({V}));
  return Out;
}

TEST(Indicators, Sma) {
  expectSeries(runSingle("sma", 3, kClose),
               {kNa, kNa, 11, 11.3333333333, 12, 12.6666666667, 13.3333333333,
                14, 14.6666666667, 15.3333333333});
}

TEST(Indicators, Ema) {
  expectSeries(runSingle("ema", 3, kClose),
               {kNa, kNa, 11, 11, 12, 13, 13, 14, 15, 15});
}

TEST(Indicators, Rsi) {
  expectSeries(runSingle("rsi", 3, kClose),
               {kNa, kNa, kNa, 66.6666666667, 83.3333333333, 87.8787878788,
                62.3655913978, 79.8850574713, 85.0905218317, 61.2965093978});
}

TEST(Indicators, RsiOnlyGrowthIs100AndConstantIs50) {
  expectSeries(runSingle("rsi", 3, {1, 2, 3, 4, 5}), {kNa, kNa, kNa, 100, 100});
  expectSeries(runSingle("rsi", 3, {7, 7, 7, 7, 7}), {kNa, kNa, kNa, 50, 50});
}

TEST(Indicators, Atr) {
  auto Ind = makeIndicator("atr", {3});
  const auto High = shifted(1);
  const auto Low = shifted(-1);
  std::vector<Value> Out;
  for (std::size_t I = 0; I < kClose.size(); ++I)
    Out.push_back(Ind->update({High[I], Low[I], kClose[I]}));
  expectSeries(Out, {kNa, kNa, 2, 2, 2.3333333333, 2.2222222222, 2.1481481481,
                     2.4320987654, 2.2880658436, 2.1920438957});
}

TEST(Indicators, Stdev) {
  expectSeries(runSingle("stdev", 3, kClose),
               {kNa, kNa, 0.8164965809, 0.4714045208, 0.8164965809,
                1.2472191289, 0.4714045208, 0.8164965809, 1.2472191289,
                0.4714045208});
}

TEST(Indicators, HighestLowestIncludeCurrent) {
  expectSeries(runSingle("highest", 3, kClose),
               {kNa, kNa, 12, 12, 13, 14, 14, 15, 16, 16});
  expectSeries(runSingle("lowest", 3, kClose),
               {kNa, kNa, 10, 11, 11, 11, 13, 13, 13, 15});
}

TEST(Indicators, Change) {
  expectSeries(runSingle("change", 2, kClose),
               {kNa, kNa, 2, 0, 1, 3, 0, 1, 3, 0});
}

TEST(Indicators, CrossoverAndCrossunder) {
  auto Over = makeIndicator("crossover", {});
  const std::vector<double> A = {1, 2, 3};
  const std::vector<double> B = {2, 2, 2};
  std::vector<Value> Out;
  for (std::size_t I = 0; I < A.size(); ++I)
    Out.push_back(Over->update({A[I], B[I]}));
  expectSeries(Out, {0, 0, 1});

  auto Under = makeIndicator("crossunder", {});
  Out.clear();
  for (std::size_t I = 0; I < A.size(); ++I)
    Out.push_back(Under->update({B[I], A[I]}));
  expectSeries(Out, {0, 0, 1});
}

TEST(Indicators, CrossoverOnTouchNeedsStrictBreakout) {
  // a[1] <= b[1] и a > b: касание на баре 1 не считается пересечением, пробой
  // на баре 2 — считается.
  auto Over = makeIndicator("crossover", {});
  EXPECT_EQ(Over->update({1.0, 2.0}), Value(0.0));
  EXPECT_EQ(Over->update({2.0, 2.0}), Value(0.0));
  EXPECT_EQ(Over->update({3.0, 2.0}), Value(1.0));
  EXPECT_EQ(Over->update({4.0, 2.0}), Value(0.0));
}

TEST(Indicators, NaInputKeepsStateAndGivesNa) {
  auto WithGap = makeIndicator("sma", {3});
  auto Plain = makeIndicator("sma", {3});
  for (double V : {10.0, 11.0}) {
    WithGap->update({V});
    Plain->update({V});
  }
  EXPECT_EQ(WithGap->update({kNa}), kNa);
  EXPECT_EQ(WithGap->update({kNa}), kNa);
  const Value A = WithGap->update({12.0});
  const Value B = Plain->update({12.0});
  ASSERT_TRUE(A && B);
  EXPECT_EQ(*A, *B);

  auto Rsi = makeIndicator("rsi", {3});
  Rsi->update({1.0});
  EXPECT_EQ(Rsi->update({kNa}), kNa);

  auto Atr = makeIndicator("atr", {3});
  EXPECT_EQ(Atr->update({kNa, 1.0, 1.0}), kNa);

  auto Over = makeIndicator("crossover", {});
  Over->update({1.0, 2.0});
  EXPECT_EQ(Over->update({kNa, 2.0}), kNa);
  EXPECT_EQ(Over->update({3.0, 2.0}), Value(1.0));
}

TEST(Indicators, PeriodOneIsAccepted) {
  expectSeries(runSingle("sma", 1, {3, 5}), {3, 5});
  expectSeries(runSingle("ema", 1, {3, 5}), {3, 5});
  expectSeries(runSingle("stdev", 1, {3, 5}), {0, 0});
  expectSeries(runSingle("rsi", 1, {3, 5, 4}), {kNa, 100, 0});
}

TEST(Indicators, Deterministic) {
  auto Run = [] {
    std::vector<Value> Out;
    for (const char *Name : {"sma", "ema", "rsi", "stdev"}) {
      for (const auto &V : runSingle(Name, 3, kClose))
        Out.push_back(V);
    }
    return Out;
  };
  EXPECT_EQ(Run(), Run());
}

TEST(Indicators, InvalidArguments) {
  EXPECT_THROW(makeIndicator("nope", {3}), std::invalid_argument);
  EXPECT_THROW(makeIndicator("sma", {}), std::invalid_argument);
  EXPECT_THROW(makeIndicator("sma", {0}), std::invalid_argument);
  EXPECT_THROW(makeIndicator("sma", {-2}), std::invalid_argument);
  EXPECT_THROW(makeIndicator("sma", {3, 4}), std::invalid_argument);
  EXPECT_THROW(makeIndicator("crossover", {3}), std::invalid_argument);
  auto Sma = makeIndicator("sma", {3});
  EXPECT_THROW(Sma->update({1.0, 2.0}), std::invalid_argument);
}

} // namespace
