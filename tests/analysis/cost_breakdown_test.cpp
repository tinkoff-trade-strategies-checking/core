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

constexpr const char* kRules =
    "rule \"b\" when close > close[1] and position == 0 then buy 10 lots because \"рост\"\n"
    "rule \"s\" when close < close[1] and position > 0 then sell all because \"падение\"\n";

Decimal netOf(const SessionSpec& spec, const std::shared_ptr<const Timeline>& timeline,
              const testing::LoadedProgram& loaded) {
  return runBacktest(spec, timeline, loaded.program, loaded.params, {}, dsl::Level::Ok, nullptr).finalEquity -
         spec.initialCash;
}

TEST(CostBreakdown, SixStepsInOrder) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto loaded = loadProgram(kRules);
  const CostBreakdown breakdown = costBreakdown(spec, timeline, loaded.program, loaded.params, {}, std::nullopt);

  const std::vector<CostFactor> order{CostFactor::Naive,  CostFactor::Delay,    CostFactor::Fees,
                                      CostFactor::Spread, CostFactor::Slippage, CostFactor::Liquidity};
  ASSERT_EQ(breakdown.steps.size(), order.size());
  for (std::size_t i = 0; i < order.size(); ++i) EXPECT_EQ(breakdown.steps[i].factor, order[i]) << i;

  // Каждый шаг - отдельный прогон с накопленными факторами.
  SessionSpec s = spec;
  s.naive = true;
  EXPECT_EQ(breakdown.steps[0].netResult, netOf(s, timeline, loaded));
  s.naive = false;
  s.costs = CostConfig::zero();
  EXPECT_EQ(breakdown.steps[1].netResult, netOf(s, timeline, loaded));
  s.costs.commissionRate = spec.costs.commissionRate;
  s.costs.exchangeFeeRate = spec.costs.exchangeFeeRate;
  EXPECT_EQ(breakdown.steps[2].netResult, netOf(s, timeline, loaded));
  s.costs.spread = spec.costs.spread;
  EXPECT_EQ(breakdown.steps[3].netResult, netOf(s, timeline, loaded));
  s.costs.slippageKappa = spec.costs.slippageKappa;
  EXPECT_EQ(breakdown.steps[4].netResult, netOf(s, timeline, loaded));

  // Наивный и честный результаты различаются (стратегия торгует), доходность = результат / капитал.
  EXPECT_NE(breakdown.steps.front().netResult, breakdown.steps.back().netResult);
  for (const CostBreakdownStep& step : breakdown.steps)
    EXPECT_DOUBLE_EQ(step.netReturn, step.netResult.toDouble() / 1'000'000.0);
}

TEST(CostBreakdown, LastEqualsHonestRun) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto loaded = loadProgram(kRules);
  const CostBreakdown breakdown = costBreakdown(spec, timeline, loaded.program, loaded.params, {}, std::nullopt);
  EXPECT_EQ(breakdown.steps.back().netResult, netOf(spec, timeline, loaded));
}

// INV-3: последний этап идёт с теми же настройками, что и честный прогон, при любых издержках honest.
TEST(CostBreakdown, LastEqualsHonestRunForCustomCosts) {
  auto [spec, timeline] = testing::fixtureA();
  spec.costs.commissionRate = dec("0.001");
  spec.costs.exchangeFeeRate = dec("0.0003");
  spec.costs.spread = dec("0.004");
  spec.costs.slippageKappa = dec("0.5");
  spec.costs.liquidityShare = dec("0.0005");  // 1000000 * 0.0005 -> лимит режет заявки
  const auto loaded = loadProgram(kRules);
  const CostBreakdown breakdown = costBreakdown(spec, timeline, loaded.program, loaded.params, {}, std::nullopt);
  EXPECT_EQ(breakdown.steps.back().netResult, netOf(spec, timeline, loaded));
}

TEST(CostBreakdown, NoLiquidityLimitInHonest) {
  auto [spec, timeline] = testing::fixtureA();
  spec.costs.liquidityShare = std::nullopt;
  const auto loaded = loadProgram(kRules);
  const CostBreakdown breakdown = costBreakdown(spec, timeline, loaded.program, loaded.params, {}, std::nullopt);
  ASSERT_EQ(breakdown.steps.size(), 6u);
  EXPECT_EQ(breakdown.steps.back().netResult, netOf(spec, timeline, loaded));
  // Лимита нет ни в честном, ни в предыдущем этапе - результаты совпадают.
  EXPECT_EQ(breakdown.steps[5].netResult, breakdown.steps[4].netResult);
}

TEST(CostBreakdown, FlagsMatchCostFlagsOnRealRuns) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto loaded = loadProgram(kRules);
  for (const std::optional<double> sharpe : {std::optional<double>{}, std::optional<double>{2.0},
                                             std::optional<double>{3.5}}) {
    const CostBreakdown b = costBreakdown(spec, timeline, loaded.program, loaded.params, {}, sharpe);
    EXPECT_EQ(b.flags, costFlags(b.steps.front().netResult, b.steps.back().netResult, sharpe));
    const bool over3 = std::find(b.flags.begin(), b.flags.end(), CostFlag::SharpeOver3) != b.flags.end();
    EXPECT_EQ(over3, sharpe && *sharpe > 3.0);
  }
}

TEST(CostBreakdown, DetailedReportsStageStatuses) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto loaded = loadProgram(kRules);
  const CostBreakdownDetail d = costBreakdownDetailed(spec, timeline, loaded.program, loaded.params, {}, std::nullopt);
  ASSERT_EQ(d.stages.size(), d.breakdown.steps.size());
  for (const CostStageRun& stage : d.stages) EXPECT_EQ(stage.status, RunStatus::Completed);
  const CostBreakdown plain = costBreakdown(spec, timeline, loaded.program, loaded.params, {}, std::nullopt);
  EXPECT_EQ(plain.steps.back().netResult, d.breakdown.steps.back().netResult);
}

TEST(CostBreakdown, Deterministic) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto loaded = loadProgram(kRules);
  const CostBreakdown a = costBreakdown(spec, timeline, loaded.program, loaded.params, {}, 1.0);
  const CostBreakdown b = costBreakdown(spec, timeline, loaded.program, loaded.params, {}, 1.0);
  ASSERT_EQ(a.steps.size(), b.steps.size());
  for (std::size_t i = 0; i < a.steps.size(); ++i) {
    EXPECT_EQ(a.steps[i].netResult, b.steps[i].netResult);
    EXPECT_EQ(a.steps[i].netReturn, b.steps[i].netReturn);
  }
}

TEST(CostBreakdown, NaiveHonestSpecRejected) {
  auto [spec, timeline] = testing::fixtureA();
  spec.naive = true;
  const auto loaded = loadProgram(kRules);
  EXPECT_THROW(costBreakdown(spec, timeline, loaded.program, loaded.params, {}, std::nullopt), std::invalid_argument);
}

TEST(CostBreakdown, Flags) {
  using Flags = std::vector<CostFlag>;
  // Издержки съели больше половины: 1000 -> 400.
  EXPECT_EQ(costFlags(dec("1000"), dec("400"), std::nullopt), Flags{CostFlag::CostsOverHalfProfit});
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
  EXPECT_EQ(costFlags(dec("-100"), dec("50"), std::nullopt), Flags{CostFlag::SignChange});
  EXPECT_EQ(costFlags(dec("0"), dec("0"), std::nullopt), Flags{});
  // Sharpe строго больше 3.
  EXPECT_EQ(costFlags(dec("1000"), dec("990"), 3.2), Flags{CostFlag::SharpeOver3});
  EXPECT_EQ(costFlags(dec("1000"), dec("990"), 3.0), Flags{});
}

}  // namespace
}  // namespace core
