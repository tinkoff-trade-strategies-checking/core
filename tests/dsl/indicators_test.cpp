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

std::vector<double> shifted(double delta) {
  std::vector<double> out;
  for (double c : kClose)
    out.push_back(c + delta);
  return out;
}

void expectSeries(const std::vector<Value> &actual,
                  const std::vector<Value> &expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    ASSERT_EQ(actual[i].has_value(), expected[i].has_value()) << "бар " << i;
    if (expected[i]) {
      EXPECT_NEAR(*actual[i], *expected[i], kEps) << "бар " << i;
    }
  }
}

std::vector<Value> runSingle(const char *name, std::int64_t n,
                             const std::vector<double> &x) {
  auto ind = makeIndicator(name, {n});
  std::vector<Value> out;
  for (double v : x)
    out.push_back(ind->update({v}));
  return out;
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
  auto ind = makeIndicator("atr", {3});
  const auto high = shifted(1);
  const auto low = shifted(-1);
  std::vector<Value> out;
  for (std::size_t i = 0; i < kClose.size(); ++i)
    out.push_back(ind->update({high[i], low[i], kClose[i]}));
  expectSeries(out, {kNa, kNa, 2, 2, 2.3333333333, 2.2222222222, 2.1481481481,
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
  auto over = makeIndicator("crossover", {});
  const std::vector<double> a = {1, 2, 3};
  const std::vector<double> b = {2, 2, 2};
  std::vector<Value> out;
  for (std::size_t i = 0; i < a.size(); ++i)
    out.push_back(over->update({a[i], b[i]}));
  expectSeries(out, {0, 0, 1});

  auto under = makeIndicator("crossunder", {});
  out.clear();
  for (std::size_t i = 0; i < a.size(); ++i)
    out.push_back(under->update({b[i], a[i]}));
  expectSeries(out, {0, 0, 1});
}

TEST(Indicators, CrossoverOnTouchNeedsStrictBreakout) {
  // a[1] <= b[1] и a > b: касание на баре 1 не считается пересечением, пробой
  // на баре 2 — считается.
  auto over = makeIndicator("crossover", {});
  EXPECT_EQ(over->update({1.0, 2.0}), Value(0.0));
  EXPECT_EQ(over->update({2.0, 2.0}), Value(0.0));
  EXPECT_EQ(over->update({3.0, 2.0}), Value(1.0));
  EXPECT_EQ(over->update({4.0, 2.0}), Value(0.0));
}

TEST(Indicators, NaInputKeepsStateAndGivesNa) {
  auto withGap = makeIndicator("sma", {3});
  auto plain = makeIndicator("sma", {3});
  for (double v : {10.0, 11.0}) {
    withGap->update({v});
    plain->update({v});
  }
  EXPECT_EQ(withGap->update({kNa}), kNa);
  EXPECT_EQ(withGap->update({kNa}), kNa);
  const Value a = withGap->update({12.0});
  const Value b = plain->update({12.0});
  ASSERT_TRUE(a && b);
  EXPECT_EQ(*a, *b);

  auto rsi = makeIndicator("rsi", {3});
  rsi->update({1.0});
  EXPECT_EQ(rsi->update({kNa}), kNa);

  auto atr = makeIndicator("atr", {3});
  EXPECT_EQ(atr->update({kNa, 1.0, 1.0}), kNa);

  auto over = makeIndicator("crossover", {});
  over->update({1.0, 2.0});
  EXPECT_EQ(over->update({kNa, 2.0}), kNa);
  EXPECT_EQ(over->update({3.0, 2.0}), Value(1.0));
}

TEST(Indicators, PeriodOneIsAccepted) {
  expectSeries(runSingle("sma", 1, {3, 5}), {3, 5});
  expectSeries(runSingle("ema", 1, {3, 5}), {3, 5});
  expectSeries(runSingle("stdev", 1, {3, 5}), {0, 0});
  expectSeries(runSingle("rsi", 1, {3, 5, 4}), {kNa, 100, 0});
}

TEST(Indicators, Deterministic) {
  auto run = [] {
    std::vector<Value> out;
    for (const char *name : {"sma", "ema", "rsi", "stdev"}) {
      for (const auto &v : runSingle(name, 3, kClose))
        out.push_back(v);
    }
    return out;
  };
  EXPECT_EQ(run(), run());
}

TEST(Indicators, InvalidArguments) {
  EXPECT_THROW(makeIndicator("nope", {3}), std::invalid_argument);
  EXPECT_THROW(makeIndicator("sma", {}), std::invalid_argument);
  EXPECT_THROW(makeIndicator("sma", {0}), std::invalid_argument);
  EXPECT_THROW(makeIndicator("sma", {-2}), std::invalid_argument);
  EXPECT_THROW(makeIndicator("sma", {3, 4}), std::invalid_argument);
  EXPECT_THROW(makeIndicator("crossover", {3}), std::invalid_argument);
  auto sma = makeIndicator("sma", {3});
  EXPECT_THROW(sma->update({1.0, 2.0}), std::invalid_argument);
}

} // namespace
