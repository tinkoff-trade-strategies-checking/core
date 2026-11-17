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

constexpr const char *kSmaCross =
    "param n = 5 range 2..20 step 3\n"
    "rule \"b\" when close > sma(close, n) and position == 0 then buy 10 lots "
    "because \"выше средней {n}\"\n"
    "rule \"s\" when close < sma(close, n) and position > 0 then sell all "
    "because \"ниже средней {n}\"\n";

constexpr const char *kNoParams =
    "rule \"b\" when close > close[1] and position == 0 then buy 10 lots "
    "because \"рост\"\n"
    "rule \"s\" when close < close[1] and position > 0 then sell all because "
    "\"падение\"\n";

dsl::ResolvedParam ranged(const char *Name, double From, double To,
                          double Step) {
  return dsl::ResolvedParam{Name, From, From, To, Step, false};
}

// Окна подбора и проверки из брифа задачи 28, на синтетической шкале в 400
// торговых дней.
std::pair<SessionSpec, std::shared_ptr<const Timeline>> timeline400() {
  return testing::syntheticTimeline(testing::randomWalk(400));
}

// Все значения сетки в порядке перебора: список значений параметров по порядку
// объявления.
std::vector<std::vector<double>>
gridValues(const std::vector<dsl::ResolvedParam> &Params,
           std::int32_t MaxGrid) {
  std::vector<std::vector<double>> Out;
  for (const dsl::ParamValues &Values : paramGrid(Params, MaxGrid)) {
    std::vector<double> Row;
    for (const dsl::ResolvedParam &P : Params)
      Row.push_back(Values.at(P.Name));
    Out.push_back(Row);
  }
  return Out;
}

TEST(ParamGrid, Order) {
  const std::vector<dsl::ResolvedParam> Params{ranged("a", 1, 2, 1),
                                               ranged("b", 10, 20, 10)};
  const std::vector<std::vector<double>> Expected{
      {1, 10}, {1, 20}, {2, 10}, {2, 20}};
  EXPECT_EQ(gridValues(Params, 200), Expected);
}

TEST(ParamGrid, ParamWithoutRangeUsesDefault) {
  const std::vector<dsl::ResolvedParam> Params{
      ranged("a", 1, 2, 1), dsl::ResolvedParam{"c", 7, {}, {}, {}, false},
      ranged("b", 10, 20, 10)};
  const std::vector<std::vector<double>> Expected{
      {1, 7, 10}, {1, 7, 20}, {2, 7, 10}, {2, 7, 20}};
  EXPECT_EQ(gridValues(Params, 200), Expected);
}

TEST(ParamGrid, NoParamsIsOneEmptyCombination) {
  const std::vector<dsl::ParamValues> Grid = paramGrid({}, 200);
  ASSERT_EQ(Grid.size(), 1u);
  EXPECT_TRUE(Grid.front().empty());
}

// Сетка не накапливает ошибку double: 0.1, 0.2, 0.3 - три значения, последнее
// не больше «до».
TEST(ParamGrid, FractionalStepKeepsLastValue) {
  const std::vector<dsl::ResolvedParam> Params{ranged("x", 0.1, 0.3, 0.1)};
  const std::vector<dsl::ParamValues> Grid = paramGrid(Params, 200);
  ASSERT_EQ(Grid.size(), 3u);
  EXPECT_LE(Grid.back().at("x"), 0.3);
  EXPECT_NEAR(Grid.back().at("x"), 0.3, 1e-12);
}

TEST(ParamGrid, ExactlyMaxIsAllowed) {
  const std::vector<dsl::ResolvedParam> Params{ranged("a", 1, 10, 1),
                                               ranged("b", 1, 20, 1)};
  EXPECT_EQ(paramGrid(Params, 200).size(), 200u);
}

TEST(ParamGrid, TooLarge) {
  const std::vector<dsl::ResolvedParam> Params{ranged("a", 1, 10, 1),
                                               ranged("b", 1, 21, 1)};
  EXPECT_THROW(paramGrid(Params, 200), std::invalid_argument);
  // Огромный диапазон: отказ без построения сетки.
  EXPECT_THROW(paramGrid({ranged("a", 0, 1e18, 1)}, 200),
               std::invalid_argument);
  EXPECT_THROW(paramGrid({ranged("a", 1, 3, 1)}, 0), std::invalid_argument);
}

TEST(WalkForward, ChainedReturnFormula) {
  EXPECT_NEAR(chainedReturn({0.1, -0.05}), 0.045, 1e-12);
  EXPECT_DOUBLE_EQ(chainedReturn({}), 0.0);
}

TEST(WalkForward, Segments) {
  const auto [spec, timeline] = timeline400();
  ASSERT_EQ(timeline->tradingDays(), 400);
  const auto Loaded = loadProgram(kNoParams);
  const WalkForwardResult Result =
      walkForward(spec, timeline, Loaded.Program, Loaded.Params, {}, nullptr);

  // Третий отрезок (проверка 377-439) в 400 дней не помещается.
  ASSERT_EQ(Result.Segments.size(), 2u);
  const WalkForwardSegment &A = Result.Segments[0];
  EXPECT_EQ(A.Index, 0);
  EXPECT_EQ(A.FitFromDay, 1);
  EXPECT_EQ(A.FitToDay, 250);
  EXPECT_EQ(A.TestFromDay, 251);
  EXPECT_EQ(A.TestToDay, 313);
  const WalkForwardSegment &B = Result.Segments[1];
  EXPECT_EQ(B.Index, 1);
  EXPECT_EQ(B.FitFromDay, 64);
  EXPECT_EQ(B.FitToDay, 313);
  EXPECT_EQ(B.TestFromDay, 314);
  EXPECT_EQ(B.TestToDay, 376);

  // Проверка идёт на срезе: 63 дня, шаги с 0; отчёт посчитан по нему же.
  for (const WalkForwardSegment &S : Result.Segments) {
    EXPECT_EQ(S.Test.Status, RunStatus::Completed);
    EXPECT_EQ(S.Test.Equity.size(), 63u);
    EXPECT_EQ(S.TestReport.Stats.TradingDays, 63);
    EXPECT_EQ(S.TestReport.FinalEquity, S.Test.FinalEquity);
    EXPECT_TRUE(S.Chosen.empty());
  }
}

TEST(WalkForward, TestRunEqualsDirectRunOnSlice) {
  const auto [spec, timeline] = timeline400();
  const auto Loaded = loadProgram(kSmaCross);
  const WalkForwardResult Result =
      walkForward(spec, timeline, Loaded.Program, Loaded.Params, {}, nullptr);
  ASSERT_FALSE(Result.Segments.empty());
  for (const WalkForwardSegment &S : Result.Segments) {
    const auto Slice = std::make_shared<const Timeline>(
        timeline->slice(S.TestFromDay, S.TestToDay));
    const RunOutput Direct =
        runBacktest(spec, Slice, Loaded.Program, Loaded.Params, S.Chosen,
                    dsl::Level::Ok, nullptr);
    EXPECT_EQ(Direct.Digest, S.Test.Digest) << S.Index;
    EXPECT_EQ(Direct.FinalEquity, S.Test.FinalEquity) << S.Index;
  }
}

// Выбор - максимум net-доходности на окне подбора, найденный независимым
// перебором по сетке.
TEST(WalkForward, PicksBestOnFitWindow) {
  const auto [spec, timeline] = timeline400();
  const auto Loaded = loadProgram(kSmaCross);
  const std::vector<dsl::ParamValues> Grid = paramGrid(Loaded.Params, 200);
  ASSERT_EQ(Grid.size(), 7u); // 2, 5, 8, 11, 14, 17, 20
  const WalkForwardResult Result =
      walkForward(spec, timeline, Loaded.Program, Loaded.Params, {}, nullptr);

  for (const WalkForwardSegment &S : Result.Segments) {
    const auto Fit = std::make_shared<const Timeline>(
        timeline->slice(S.FitFromDay, S.FitToDay));
    double Best = 0;
    double ChosenReturn = 0;
    std::vector<double> Returns;
    for (const dsl::ParamValues &Values : Grid) {
      const RunOutput Out =
          runBacktest(spec, Fit, Loaded.Program, Loaded.Params, Values,
                      dsl::Level::Ok, nullptr);
      const double R = (Out.FinalEquity - spec.InitialCash).toDouble() /
                       spec.InitialCash.toDouble();
      Returns.push_back(R);
      if (Values == S.Chosen)
        ChosenReturn = R;
    }
    Best = *std::max_element(Returns.begin(), Returns.end());
    EXPECT_DOUBLE_EQ(S.FitNetReturn, Best) << S.Index;
    EXPECT_DOUBLE_EQ(ChosenReturn, Best) << S.Index;
    // Подбор содержательный: доходности на сетке различаются.
    EXPECT_NE(*std::min_element(Returns.begin(), Returns.end()), Best)
        << S.Index;
  }
}

// Параметры не влияют на сделки: все наборы дают одну доходность, берётся
// первый в порядке сетки.
TEST(WalkForward, TieBreak) {
  const auto [spec, timeline] = timeline400();
  const std::string Source = std::string("param a = 2 range 1..3 step 1\nparam "
                                         "b = 20 range 10..30 step 10\n") +
                             kNoParams;
  const auto Loaded = loadProgram(Source);
  const WalkForwardResult Result =
      walkForward(spec, timeline, Loaded.Program, Loaded.Params, {}, nullptr);
  ASSERT_EQ(Result.Segments.size(), 2u);
  for (const WalkForwardSegment &S : Result.Segments) {
    EXPECT_EQ(S.Chosen, (dsl::ParamValues{{"a", 1}, {"b", 10}})) << S.Index;
  }
}

TEST(WalkForward, ChainedReturn) {
  const auto [spec, timeline] = timeline400();
  const auto Loaded = loadProgram(kSmaCross);
  const WalkForwardResult Result =
      walkForward(spec, timeline, Loaded.Program, Loaded.Params, {}, nullptr);
  std::vector<double> Returns;
  for (const WalkForwardSegment &S : Result.Segments) {
    Returns.push_back((S.Test.FinalEquity - spec.InitialCash).toDouble() /
                      spec.InitialCash.toDouble());
  }
  EXPECT_DOUBLE_EQ(Result.ChainedReturn, chainedReturn(Returns));
}

TEST(WalkForward, ProgressCoversAllRuns) {
  const auto [spec, timeline] = timeline400();
  const auto Loaded = loadProgram(kSmaCross); // 7 наборов
  std::vector<std::pair<std::int64_t, std::int64_t>> Calls;
  const WalkForwardResult Result =
      walkForward(spec, timeline, Loaded.Program, Loaded.Params, {},
                  [&](std::int64_t Done, std::int64_t Total) {
                    Calls.emplace_back(Done, Total);
                  });
  ASSERT_EQ(Result.Segments.size(), 2u);
  // По шагу на день: 2 · (7 · 250 + 63) прогонов-шагов.
  const std::int64_t ExpectedTotal = 2 * (7 * 250 + 63);
  ASSERT_FALSE(Calls.empty());
  std::int64_t Previous = 0;
  for (const auto &[done, total] : Calls) {
    EXPECT_EQ(total, ExpectedTotal);
    EXPECT_GT(done, Previous);
    Previous = done;
  }
  EXPECT_EQ(Calls.back().first, ExpectedTotal);
}

TEST(WalkForward, SameResultTwice) {
  const auto [spec, timeline] = timeline400();
  const auto Loaded = loadProgram(kSmaCross);
  const WalkForwardResult First =
      walkForward(spec, timeline, Loaded.Program, Loaded.Params, {}, nullptr);
  const WalkForwardResult Second =
      walkForward(spec, timeline, Loaded.Program, Loaded.Params, {}, nullptr);
  ASSERT_EQ(First.Segments.size(), Second.Segments.size());
  for (std::size_t I = 0; I < First.Segments.size(); ++I) {
    EXPECT_EQ(First.Segments[I].Chosen, Second.Segments[I].Chosen);
    EXPECT_EQ(First.Segments[I].Test.Digest, Second.Segments[I].Test.Digest);
  }
  EXPECT_DOUBLE_EQ(First.ChainedReturn, Second.ChainedReturn);
}

TEST(WalkForward, CustomWindows) {
  const auto [spec, timeline] = timeline400();
  const auto Loaded = loadProgram(kNoParams);
  const WalkForwardResult Result =
      walkForward(spec, timeline, Loaded.Program, Loaded.Params,
                  WalkForwardConfig{100, 50, 100, 200}, nullptr);
  // Подбор 1-100 / проверка 101-150, подбор 101-200 / 201-250, 201-300 /
  // 301-350; четвёртый (301-400 / 401-450) не помещается.
  ASSERT_EQ(Result.Segments.size(), 3u);
  EXPECT_EQ(Result.Segments[2].FitFromDay, 201);
  EXPECT_EQ(Result.Segments[2].TestToDay, 350);
}

TEST(WalkForward, Refusals) {
  const auto [spec, timeline] = timeline400();
  const auto Loaded = loadProgram(kSmaCross);
  // Окна длиннее шкалы: ни одного отрезка.
  EXPECT_THROW(walkForward(spec, timeline, Loaded.Program, Loaded.Params,
                           WalkForwardConfig{380, 63, 63, 200}, nullptr),
               std::invalid_argument);
  EXPECT_THROW(walkForward(spec, timeline, Loaded.Program, Loaded.Params,
                           WalkForwardConfig{0, 63, 63, 200}, nullptr),
               std::invalid_argument);
  EXPECT_THROW(walkForward(spec, timeline, Loaded.Program, Loaded.Params,
                           WalkForwardConfig{250, 63, 0, 200}, nullptr),
               std::invalid_argument);
  // Сетка больше предела отклоняется до первого прогона.
  std::int32_t Calls = 0;
  EXPECT_THROW(walkForward(spec, timeline, Loaded.Program, Loaded.Params,
                           WalkForwardConfig{250, 63, 63, 3},
                           [&](std::int64_t, std::int64_t) { ++Calls; }),
               std::invalid_argument);
  EXPECT_EQ(Calls, 0);
}

// Прогрев: у каждого окна (подбора и проверки) - W шагов истории прямо перед
// ним, прогон окна равен прямому прогону на срезе с тем же прогревом.
TEST(WalkForward, WarmupGoesIntoEveryWindow) {
  const auto [spec, full] = timeline400();
  const Timeline History = full->slice(1, 30);
  const auto Warmed = std::make_shared<const Timeline>(
      full->slice(31, 400).withWarmup(History, 20));
  ASSERT_EQ(Warmed->warmupSteps(), 20);
  const auto Loaded = loadProgram(kSmaCross);
  const WalkForwardResult Result =
      walkForward(spec, Warmed, Loaded.Program, Loaded.Params, {}, nullptr);
  ASSERT_FALSE(Result.Segments.empty());
  for (const WalkForwardSegment &S : Result.Segments) {
    const auto Slice = std::make_shared<const Timeline>(
        Warmed->slice(S.TestFromDay, S.TestToDay));
    EXPECT_EQ(Slice->warmupSteps(), 20) << S.Index;
    const RunOutput Direct =
        runBacktest(spec, Slice, Loaded.Program, Loaded.Params, S.Chosen,
                    dsl::Level::Ok, nullptr);
    EXPECT_EQ(Direct.Digest, S.Test.Digest) << S.Index;
  }
  // Первое окно подбора начинается с истории до периода: её последний бар -
  // день 30 исходной шкалы.
  const Timeline FirstFit =
      Warmed->slice(Result.Segments[0].FitFromDay, Result.Segments[0].FitToDay);
  EXPECT_EQ(FirstFit.warmupBars().back().Close, full->barsAt(29).back().Close);
}

} // namespace
} // namespace core
