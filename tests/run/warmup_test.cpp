#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "core/run/backtest.hpp"
#include "run/run_test_support.hpp"

// Прогон стратегии с прогревом (Timeline::warmupBars): индикаторы готовы к шагу
// 0, торговли до шага 0 нет.

namespace core {
namespace {

using testing::fixture1;
using testing::loadProgram;

// Бумага A fixture-1 с 11 января (close 103, 106, 104, 101, 99, 102, 105, 108);
// прогрев - 9 и 10 января (100, 101).
std::shared_ptr<const Timeline> fromJan11(std::int64_t Warmup) {
  auto F = fixture1();
  const std::vector<Instrument> Instruments{F.Instruments[0]};
  return std::make_shared<const Timeline>(
      Timeline::loadWithWarmup(F.Bars, F.Calendar, F.SnapshotId, Instruments,
                               Timeframe::D1, 20240111, 20240122, Warmup));
}

RunOutput run(const std::shared_ptr<const Timeline> &Timeline) {
  // close[2] на шаге 0 есть только с прогревом.
  const auto Loaded =
      loadProgram("rule \"b\" when close > close[2] and position == 0 then buy "
                  "10 lots because \"выше, чем два бара назад\"\n");
  auto F = fixture1();
  return runBacktest(testing::specFor({F.Instruments[0]}), Timeline,
                     Loaded.Program, Loaded.Params, {}, dsl::Level::Ok,
                     nullptr);
}

TEST(Warmup, StrategySeesHistoryFromStepZero) {
  const RunOutput Plain = run(fromJan11(0));
  ASSERT_FALSE(Plain.Fills.empty());
  // Без истории: первое сравнение на шаге 2 (104 > 103), исполнение по open
  // шага 3.
  EXPECT_EQ(Plain.Fills.front().Step, 3);

  const RunOutput Warm = run(fromJan11(2));
  ASSERT_FALSE(Warm.Fills.empty());
  // С прогревом: уже на шаге 0 103 > 100, исполнение по open шага 1 (INV-2).
  EXPECT_EQ(Warm.Fills.front().Step, 1);
}

TEST(Warmup, NoTradingAndNoEquityBeforeStepZero) {
  const auto Timeline = fromJan11(2);
  const RunOutput Out = run(Timeline);
  ASSERT_EQ(Out.Equity.size(),
            static_cast<std::size_t>(Timeline->totalSteps()));
  EXPECT_EQ(Out.Equity.front().Step, 0);
  EXPECT_EQ(Out.Equity.front().Equity, Decimal::parse("1000000"));
  for (const Fill &F : Out.Fills)
    EXPECT_GE(F.Step, 1);
  for (const Order &O : Out.Orders)
    EXPECT_GE(O.PlacedStep, 0);
}

TEST(Warmup, Deterministic) {
  EXPECT_EQ(run(fromJan11(2)).Digest, run(fromJan11(2)).Digest);
  EXPECT_NE(run(fromJan11(2)).Digest, run(fromJan11(0)).Digest);
}

} // namespace
} // namespace core
