#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "core/run/backtest.hpp"
#include "run/run_test_support.hpp"

// Прогон стратегии с прогревом (Timeline::warmupBars): индикаторы готовы к шагу 0, торговли до шага 0 нет.

namespace core {
namespace {

using testing::fixture1;
using testing::loadProgram;

// Бумага A fixture-1 с 11 января (close 103, 106, 104, 101, 99, 102, 105, 108); прогрев - 9 и 10 января (100, 101).
std::shared_ptr<const Timeline> fromJan11(std::int64_t warmup) {
  auto f = fixture1();
  const std::vector<Instrument> instruments{f.instruments[0]};
  return std::make_shared<const Timeline>(Timeline::loadWithWarmup(f.bars, f.calendar, f.snapshotId, instruments,
                                                                   Timeframe::D1, 20240111, 20240122, warmup));
}

RunOutput run(const std::shared_ptr<const Timeline>& timeline) {
  // close[2] на шаге 0 есть только с прогревом.
  const auto loaded = loadProgram(
      "rule \"b\" when close > close[2] and position == 0 then buy 10 lots because \"выше, чем два бара назад\"\n");
  auto f = fixture1();
  return runBacktest(testing::specFor({f.instruments[0]}), timeline, loaded.program, loaded.params, {},
                     dsl::Level::Ok, nullptr);
}

TEST(Warmup, StrategySeesHistoryFromStepZero) {
  const RunOutput plain = run(fromJan11(0));
  ASSERT_FALSE(plain.fills.empty());
  // Без истории: первое сравнение на шаге 2 (104 > 103), исполнение по open шага 3.
  EXPECT_EQ(plain.fills.front().step, 3);

  const RunOutput warm = run(fromJan11(2));
  ASSERT_FALSE(warm.fills.empty());
  // С прогревом: уже на шаге 0 103 > 100, исполнение по open шага 1 (INV-2).
  EXPECT_EQ(warm.fills.front().step, 1);
}

TEST(Warmup, NoTradingAndNoEquityBeforeStepZero) {
  const auto timeline = fromJan11(2);
  const RunOutput out = run(timeline);
  ASSERT_EQ(out.equity.size(), static_cast<std::size_t>(timeline->totalSteps()));
  EXPECT_EQ(out.equity.front().step, 0);
  EXPECT_EQ(out.equity.front().equity, Decimal::parse("1000000"));
  for (const Fill& f : out.fills) EXPECT_GE(f.step, 1);
  for (const Order& o : out.orders) EXPECT_GE(o.placedStep, 0);
}

TEST(Warmup, Deterministic) {
  EXPECT_EQ(run(fromJan11(2)).digest, run(fromJan11(2)).digest);
  EXPECT_NE(run(fromJan11(2)).digest, run(fromJan11(0)).digest);
}

}  // namespace
}  // namespace core
