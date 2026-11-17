#include "core/analysis/cost_breakdown.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <vector>

#include "core/run/backtest.hpp"
#include "run/run_test_support.hpp"

namespace core {
namespace {

using testing::dec;
using testing::loadProgram;

constexpr const char *kRules = "rule \"b\" when close > close[1] and position "
                               "== 0 then buy 10 lots because \"рост\"\n"
                               "rule \"s\" when close < close[1] and position "
                               "> 0 then sell all because \"падение\"\n";

Decimal netOf(const SessionSpec &Spec,
              const std::shared_ptr<const Timeline> &Timeline,
              const testing::LoadedProgram &Loaded) {
  return runBacktest(Spec, Timeline, Loaded.Program, Loaded.Params, {},
                     dsl::Level::Ok, nullptr)
             .FinalEquity -
         Spec.InitialCash;
}

TEST(CostBreakdown, SixStepsInOrder) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto Loaded = loadProgram(kRules);
  const CostBreakdown Breakdown = costBreakdown(
      spec, timeline, Loaded.Program, Loaded.Params, {}, std::nullopt);

  const std::vector<CostFactor> Order{
      CostFactor::Naive,  CostFactor::Delay,    CostFactor::Fees,
      CostFactor::Spread, CostFactor::Slippage, CostFactor::Liquidity};
  ASSERT_EQ(Breakdown.Steps.size(), Order.size());
  for (std::size_t I = 0; I < Order.size(); ++I)
    EXPECT_EQ(Breakdown.Steps[I].Factor, Order[I]) << I;

  // Каждый шаг - отдельный прогон с накопленными факторами.
  SessionSpec S = spec;
  S.Naive = true;
  EXPECT_EQ(Breakdown.Steps[0].NetResult, netOf(S, timeline, Loaded));
  S.Naive = false;
  S.Costs = CostConfig::zero();
  EXPECT_EQ(Breakdown.Steps[1].NetResult, netOf(S, timeline, Loaded));
  S.Costs.CommissionRate = spec.Costs.CommissionRate;
  S.Costs.ExchangeFeeRate = spec.Costs.ExchangeFeeRate;
  EXPECT_EQ(Breakdown.Steps[2].NetResult, netOf(S, timeline, Loaded));
  S.Costs.Spread = spec.Costs.Spread;
  EXPECT_EQ(Breakdown.Steps[3].NetResult, netOf(S, timeline, Loaded));
  S.Costs.SlippageKappa = spec.Costs.SlippageKappa;
  EXPECT_EQ(Breakdown.Steps[4].NetResult, netOf(S, timeline, Loaded));

  // Наивный и честный результаты различаются (стратегия торгует), доходность =
  // результат / капитал.
  EXPECT_NE(Breakdown.Steps.front().NetResult,
            Breakdown.Steps.back().NetResult);
  for (const CostBreakdownStep &Step : Breakdown.Steps)
    EXPECT_DOUBLE_EQ(Step.NetReturn, Step.NetResult.toDouble() / 1'000'000.0);
}

TEST(CostBreakdown, LastEqualsHonestRun) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto Loaded = loadProgram(kRules);
  const CostBreakdown Breakdown = costBreakdown(
      spec, timeline, Loaded.Program, Loaded.Params, {}, std::nullopt);
  EXPECT_EQ(Breakdown.Steps.back().NetResult, netOf(spec, timeline, Loaded));
}

// INV-3: последний этап идёт с теми же настройками, что и честный прогон, при
// любых издержках honest.
TEST(CostBreakdown, LastEqualsHonestRunForCustomCosts) {
  auto [spec, timeline] = testing::fixtureA();
  spec.Costs.CommissionRate = dec("0.001");
  spec.Costs.ExchangeFeeRate = dec("0.0003");
  spec.Costs.Spread = dec("0.004");
  spec.Costs.SlippageKappa = dec("0.5");
  spec.Costs.LiquidityShare =
      dec("0.0005"); // 1000000 * 0.0005 -> лимит режет заявки
  const auto Loaded = loadProgram(kRules);
  const CostBreakdown Breakdown = costBreakdown(
      spec, timeline, Loaded.Program, Loaded.Params, {}, std::nullopt);
  EXPECT_EQ(Breakdown.Steps.back().NetResult, netOf(spec, timeline, Loaded));
}

TEST(CostBreakdown, NoLiquidityLimitInHonest) {
  auto [spec, timeline] = testing::fixtureA();
  spec.Costs.LiquidityShare = std::nullopt;
  const auto Loaded = loadProgram(kRules);
  const CostBreakdown Breakdown = costBreakdown(
      spec, timeline, Loaded.Program, Loaded.Params, {}, std::nullopt);
  ASSERT_EQ(Breakdown.Steps.size(), 6u);
  EXPECT_EQ(Breakdown.Steps.back().NetResult, netOf(spec, timeline, Loaded));
  // Лимита нет ни в честном, ни в предыдущем этапе - результаты совпадают.
  EXPECT_EQ(Breakdown.Steps[5].NetResult, Breakdown.Steps[4].NetResult);
}

TEST(CostBreakdown, FlagsMatchCostFlagsOnRealRuns) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto Loaded = loadProgram(kRules);
  for (const std::optional<double> Sharpe :
       {std::optional<double>{}, std::optional<double>{2.0},
        std::optional<double>{3.5}}) {
    const CostBreakdown B = costBreakdown(spec, timeline, Loaded.Program,
                                          Loaded.Params, {}, Sharpe);
    EXPECT_EQ(B.Flags, costFlags(B.Steps.front().NetResult,
                                 B.Steps.back().NetResult, Sharpe));
    const bool Over3 = std::find(B.Flags.begin(), B.Flags.end(),
                                 CostFlag::SharpeOver3) != B.Flags.end();
    EXPECT_EQ(Over3, Sharpe && *Sharpe > 3.0);
  }
}

TEST(CostBreakdown, DetailedReportsStageStatuses) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto Loaded = loadProgram(kRules);
  const CostBreakdownDetail D = costBreakdownDetailed(
      spec, timeline, Loaded.Program, Loaded.Params, {}, std::nullopt);
  ASSERT_EQ(D.Stages.size(), D.Breakdown.Steps.size());
  for (const CostStageRun &Stage : D.Stages)
    EXPECT_EQ(Stage.Status, RunStatus::Completed);
  const CostBreakdown Plain = costBreakdown(spec, timeline, Loaded.Program,
                                            Loaded.Params, {}, std::nullopt);
  EXPECT_EQ(Plain.Steps.back().NetResult, D.Breakdown.Steps.back().NetResult);
}

TEST(CostBreakdown, Deterministic) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto Loaded = loadProgram(kRules);
  const CostBreakdown A =
      costBreakdown(spec, timeline, Loaded.Program, Loaded.Params, {}, 1.0);
  const CostBreakdown B =
      costBreakdown(spec, timeline, Loaded.Program, Loaded.Params, {}, 1.0);
  ASSERT_EQ(A.Steps.size(), B.Steps.size());
  for (std::size_t I = 0; I < A.Steps.size(); ++I) {
    EXPECT_EQ(A.Steps[I].NetResult, B.Steps[I].NetResult);
    EXPECT_EQ(A.Steps[I].NetReturn, B.Steps[I].NetReturn);
  }
}

TEST(CostBreakdown, NaiveHonestSpecRejected) {
  auto [spec, timeline] = testing::fixtureA();
  spec.Naive = true;
  const auto Loaded = loadProgram(kRules);
  EXPECT_THROW(costBreakdown(spec, timeline, Loaded.Program, Loaded.Params, {},
                             std::nullopt),
               std::invalid_argument);
}

TEST(CostBreakdown, Flags) {
  using Flags = std::vector<CostFlag>;
  // Издержки съели больше половины: 1000 -> 400.
  EXPECT_EQ(costFlags(dec("1000"), dec("400"), std::nullopt),
            Flags{CostFlag::CostsOverHalfProfit});
  // Ровно половина - не больше половины.
  EXPECT_EQ(costFlags(dec("1000"), dec("500"), std::nullopt), Flags{});
  // Смена знака: 1000 -> -10.
  EXPECT_EQ(costFlags(dec("1000"), dec("-10"), std::nullopt),
            (Flags{CostFlag::CostsOverHalfProfit, CostFlag::SignChange}));
  // Нуль - отдельный знак.
  EXPECT_EQ(costFlags(dec("1000"), dec("0"), std::nullopt),
            (Flags{CostFlag::CostsOverHalfProfit, CostFlag::SignChange}));
  // Наивный убыток: первый флаг требует R_наивн > 0, смена знака - нет.
  EXPECT_EQ(costFlags(dec("-100"), dec("-300"), std::nullopt), Flags{});
  EXPECT_EQ(costFlags(dec("-100"), dec("50"), std::nullopt),
            Flags{CostFlag::SignChange});
  EXPECT_EQ(costFlags(dec("0"), dec("0"), std::nullopt), Flags{});
  // Sharpe строго больше 3.
  EXPECT_EQ(costFlags(dec("1000"), dec("990"), 3.2),
            Flags{CostFlag::SharpeOver3});
  EXPECT_EQ(costFlags(dec("1000"), dec("990"), 3.0), Flags{});
}

} // namespace
} // namespace core
