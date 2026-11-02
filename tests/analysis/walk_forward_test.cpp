#include "core/analysis/walk_forward.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "run/run_test_support.hpp"

namespace core {
namespace {

using testing::loadProgram;

constexpr const char* kSmaCross =
    "param n = 5 range 2..20 step 3\n"
    "rule \"b\" when close > sma(close, n) and position == 0 then buy 10 lots because \"выше средней {n}\"\n"
    "rule \"s\" when close < sma(close, n) and position > 0 then sell all because \"ниже средней {n}\"\n";

constexpr const char* kNoParams =
    "rule \"b\" when close > close[1] and position == 0 then buy 10 lots because \"рост\"\n"
    "rule \"s\" when close < close[1] and position > 0 then sell all because \"падение\"\n";

dsl::ResolvedParam ranged(const char* name, double from, double to, double step) {
  return dsl::ResolvedParam{name, from, from, to, step, false};
}

// Окна подбора и проверки из брифа задачи 28, на синтетической шкале в 400 торговых дней.
std::pair<SessionSpec, std::shared_ptr<const Timeline>> timeline400() {
  return testing::syntheticTimeline(testing::randomWalk(400));
}

// Все значения сетки в порядке перебора: список значений параметров по порядку объявления.
std::vector<std::vector<double>> gridValues(const std::vector<dsl::ResolvedParam>& params, std::int32_t maxGrid) {
  std::vector<std::vector<double>> out;
  for (const dsl::ParamValues& values : paramGrid(params, maxGrid)) {
    std::vector<double> row;
    for (const dsl::ResolvedParam& p : params) row.push_back(values.at(p.name));
    out.push_back(row);
  }
  return out;
}

TEST(ParamGrid, Order) {
  const std::vector<dsl::ResolvedParam> params{ranged("a", 1, 2, 1), ranged("b", 10, 20, 10)};
  const std::vector<std::vector<double>> expected{{1, 10}, {1, 20}, {2, 10}, {2, 20}};
  EXPECT_EQ(gridValues(params, 200), expected);
}

TEST(ParamGrid, ParamWithoutRangeUsesDefault) {
  const std::vector<dsl::ResolvedParam> params{ranged("a", 1, 2, 1), dsl::ResolvedParam{"c", 7, {}, {}, {}, false},
                                               ranged("b", 10, 20, 10)};
  const std::vector<std::vector<double>> expected{{1, 7, 10}, {1, 7, 20}, {2, 7, 10}, {2, 7, 20}};
  EXPECT_EQ(gridValues(params, 200), expected);
}

TEST(ParamGrid, NoParamsIsOneEmptyCombination) {
  const std::vector<dsl::ParamValues> grid = paramGrid({}, 200);
  ASSERT_EQ(grid.size(), 1u);
  EXPECT_TRUE(grid.front().empty());
}

// Сетка не накапливает ошибку double: 0.1, 0.2, 0.3 - три значения, последнее не больше «до».
TEST(ParamGrid, FractionalStepKeepsLastValue) {
  const std::vector<dsl::ResolvedParam> params{ranged("x", 0.1, 0.3, 0.1)};
  const std::vector<dsl::ParamValues> grid = paramGrid(params, 200);
  ASSERT_EQ(grid.size(), 3u);
  EXPECT_LE(grid.back().at("x"), 0.3);
  EXPECT_NEAR(grid.back().at("x"), 0.3, 1e-12);
}

TEST(ParamGrid, ExactlyMaxIsAllowed) {
  const std::vector<dsl::ResolvedParam> params{ranged("a", 1, 10, 1), ranged("b", 1, 20, 1)};
  EXPECT_EQ(paramGrid(params, 200).size(), 200u);
}

TEST(ParamGrid, TooLarge) {
  const std::vector<dsl::ResolvedParam> params{ranged("a", 1, 10, 1), ranged("b", 1, 21, 1)};
  EXPECT_THROW(paramGrid(params, 200), std::invalid_argument);
  // Огромный диапазон: отказ без построения сетки.
  EXPECT_THROW(paramGrid({ranged("a", 0, 1e18, 1)}, 200), std::invalid_argument);
  EXPECT_THROW(paramGrid({ranged("a", 1, 3, 1)}, 0), std::invalid_argument);
}

TEST(WalkForward, ChainedReturnFormula) {
  EXPECT_NEAR(chainedReturn({0.1, -0.05}), 0.045, 1e-12);
  EXPECT_DOUBLE_EQ(chainedReturn({}), 0.0);
}

TEST(WalkForward, Segments) {
  const auto [spec, timeline] = timeline400();
  ASSERT_EQ(timeline->tradingDays(), 400);
  const auto loaded = loadProgram(kNoParams);
  const WalkForwardResult result = walkForward(spec, timeline, loaded.program, loaded.params, {}, nullptr);

  // Третий отрезок (проверка 377-439) в 400 дней не помещается.
  ASSERT_EQ(result.segments.size(), 2u);
  const WalkForwardSegment& a = result.segments[0];
  EXPECT_EQ(a.index, 0);
  EXPECT_EQ(a.fitFromDay, 1);
  EXPECT_EQ(a.fitToDay, 250);
  EXPECT_EQ(a.testFromDay, 251);
  EXPECT_EQ(a.testToDay, 313);
  const WalkForwardSegment& b = result.segments[1];
  EXPECT_EQ(b.index, 1);
  EXPECT_EQ(b.fitFromDay, 64);
  EXPECT_EQ(b.fitToDay, 313);
  EXPECT_EQ(b.testFromDay, 314);
  EXPECT_EQ(b.testToDay, 376);

  // Проверка идёт на срезе: 63 дня, шаги с 0; отчёт посчитан по нему же.
  for (const WalkForwardSegment& s : result.segments) {
    EXPECT_EQ(s.test.status, RunStatus::Completed);
    EXPECT_EQ(s.test.equity.size(), 63u);
    EXPECT_EQ(s.testReport.metrics.tradingDays, 63);
    EXPECT_EQ(s.testReport.finalEquity, s.test.finalEquity);
    EXPECT_TRUE(s.chosen.empty());
  }
}

TEST(WalkForward, TestRunEqualsDirectRunOnSlice) {
  const auto [spec, timeline] = timeline400();
  const auto loaded = loadProgram(kSmaCross);
  const WalkForwardResult result = walkForward(spec, timeline, loaded.program, loaded.params, {}, nullptr);
  ASSERT_FALSE(result.segments.empty());
  for (const WalkForwardSegment& s : result.segments) {
    const auto slice = std::make_shared<const Timeline>(timeline->slice(s.testFromDay, s.testToDay));
    const RunOutput direct = runBacktest(spec, slice, loaded.program, loaded.params, s.chosen, dsl::Level::Ok, nullptr);
    EXPECT_EQ(direct.digest, s.test.digest) << s.index;
    EXPECT_EQ(direct.finalEquity, s.test.finalEquity) << s.index;
  }
}

// Выбор - максимум net-доходности на окне подбора, найденный независимым перебором по сетке.
TEST(WalkForward, PicksBestOnFitWindow) {
  const auto [spec, timeline] = timeline400();
  const auto loaded = loadProgram(kSmaCross);
  const std::vector<dsl::ParamValues> grid = paramGrid(loaded.params, 200);
  ASSERT_EQ(grid.size(), 7u);  // 2, 5, 8, 11, 14, 17, 20
  const WalkForwardResult result = walkForward(spec, timeline, loaded.program, loaded.params, {}, nullptr);

  for (const WalkForwardSegment& s : result.segments) {
    const auto fit = std::make_shared<const Timeline>(timeline->slice(s.fitFromDay, s.fitToDay));
    double best = 0;
    double chosenReturn = 0;
    std::vector<double> returns;
    for (const dsl::ParamValues& values : grid) {
      const RunOutput out = runBacktest(spec, fit, loaded.program, loaded.params, values, dsl::Level::Ok, nullptr);
      const double r = (out.finalEquity - spec.initialCash).toDouble() / spec.initialCash.toDouble();
      returns.push_back(r);
      if (values == s.chosen) chosenReturn = r;
    }
    best = *std::max_element(returns.begin(), returns.end());
    EXPECT_DOUBLE_EQ(s.fitNetReturn, best) << s.index;
    EXPECT_DOUBLE_EQ(chosenReturn, best) << s.index;
    // Подбор содержательный: доходности на сетке различаются.
    EXPECT_NE(*std::min_element(returns.begin(), returns.end()), best) << s.index;
  }
}

// Параметры не влияют на сделки: все наборы дают одну доходность, берётся первый в порядке сетки.
TEST(WalkForward, TieBreak) {
  const auto [spec, timeline] = timeline400();
  const std::string source = std::string("param a = 2 range 1..3 step 1\nparam b = 20 range 10..30 step 10\n") + kNoParams;
  const auto loaded = loadProgram(source);
  const WalkForwardResult result = walkForward(spec, timeline, loaded.program, loaded.params, {}, nullptr);
  ASSERT_EQ(result.segments.size(), 2u);
  for (const WalkForwardSegment& s : result.segments) {
    EXPECT_EQ(s.chosen, (dsl::ParamValues{{"a", 1}, {"b", 10}})) << s.index;
  }
}

TEST(WalkForward, ChainedReturn) {
  const auto [spec, timeline] = timeline400();
  const auto loaded = loadProgram(kSmaCross);
  const WalkForwardResult result = walkForward(spec, timeline, loaded.program, loaded.params, {}, nullptr);
  std::vector<double> returns;
  for (const WalkForwardSegment& s : result.segments) {
    returns.push_back((s.test.finalEquity - spec.initialCash).toDouble() / spec.initialCash.toDouble());
  }
  EXPECT_DOUBLE_EQ(result.chainedReturn, chainedReturn(returns));
}

TEST(WalkForward, ProgressCoversAllRuns) {
  const auto [spec, timeline] = timeline400();
  const auto loaded = loadProgram(kSmaCross);  // 7 наборов
  std::vector<std::pair<std::int64_t, std::int64_t>> calls;
  const WalkForwardResult result = walkForward(spec, timeline, loaded.program, loaded.params, {},
                                               [&](std::int64_t done, std::int64_t total) { calls.emplace_back(done, total); });
  ASSERT_EQ(result.segments.size(), 2u);
  // По шагу на день: 2 · (7 · 250 + 63) прогонов-шагов.
  const std::int64_t expectedTotal = 2 * (7 * 250 + 63);
  ASSERT_FALSE(calls.empty());
  std::int64_t previous = 0;
  for (const auto& [done, total] : calls) {
    EXPECT_EQ(total, expectedTotal);
    EXPECT_GT(done, previous);
    previous = done;
  }
  EXPECT_EQ(calls.back().first, expectedTotal);
}

TEST(WalkForward, SameResultTwice) {
  const auto [spec, timeline] = timeline400();
  const auto loaded = loadProgram(kSmaCross);
  const WalkForwardResult first = walkForward(spec, timeline, loaded.program, loaded.params, {}, nullptr);
  const WalkForwardResult second = walkForward(spec, timeline, loaded.program, loaded.params, {}, nullptr);
  ASSERT_EQ(first.segments.size(), second.segments.size());
  for (std::size_t i = 0; i < first.segments.size(); ++i) {
    EXPECT_EQ(first.segments[i].chosen, second.segments[i].chosen);
    EXPECT_EQ(first.segments[i].test.digest, second.segments[i].test.digest);
  }
  EXPECT_DOUBLE_EQ(first.chainedReturn, second.chainedReturn);
}

TEST(WalkForward, CustomWindows) {
  const auto [spec, timeline] = timeline400();
  const auto loaded = loadProgram(kNoParams);
  const WalkForwardResult result = walkForward(spec, timeline, loaded.program, loaded.params,
                                               WalkForwardConfig{100, 50, 100, 200}, nullptr);
  // Подбор 1-100 / проверка 101-150, подбор 101-200 / 201-250, 201-300 / 301-350; четвёртый (301-400 / 401-450) не помещается.
  ASSERT_EQ(result.segments.size(), 3u);
  EXPECT_EQ(result.segments[2].fitFromDay, 201);
  EXPECT_EQ(result.segments[2].testToDay, 350);
}

TEST(WalkForward, Refusals) {
  const auto [spec, timeline] = timeline400();
  const auto loaded = loadProgram(kSmaCross);
  // Окна длиннее шкалы: ни одного отрезка.
  EXPECT_THROW(walkForward(spec, timeline, loaded.program, loaded.params, WalkForwardConfig{380, 63, 63, 200}, nullptr),
               std::invalid_argument);
  EXPECT_THROW(walkForward(spec, timeline, loaded.program, loaded.params, WalkForwardConfig{0, 63, 63, 200}, nullptr),
               std::invalid_argument);
  EXPECT_THROW(walkForward(spec, timeline, loaded.program, loaded.params, WalkForwardConfig{250, 63, 0, 200}, nullptr),
               std::invalid_argument);
  // Сетка больше предела отклоняется до первого прогона.
  std::int32_t calls = 0;
  EXPECT_THROW(walkForward(spec, timeline, loaded.program, loaded.params, WalkForwardConfig{250, 63, 63, 3},
                           [&](std::int64_t, std::int64_t) { ++calls; }),
               std::invalid_argument);
  EXPECT_EQ(calls, 0);
}

}  // namespace
}  // namespace core
