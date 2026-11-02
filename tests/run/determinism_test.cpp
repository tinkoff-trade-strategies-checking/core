// Детерминизм (INV-4): один и тот же вход даёт побитово тот же результат прогона.
#include <gtest/gtest.h>

#include <cstdint>

#include "core/run/backtest.hpp"
#include "run/run_test_support.hpp"

namespace core {
namespace {

constexpr const char* kSmaCross =
    "param n = 5 range 2..20 step 1\n"
    "rule \"b\" when close > sma(close, n) and position == 0 then buy 10 lots because \"выше средней {n}\"\n"
    "rule \"s\" when close < sma(close, n) and position > 0 then sell all because \"ниже средней {n}\"\n";

RunOutput runSma(const dsl::ParamValues& overrides) {
  const auto [spec, timeline] = testing::syntheticTimeline(testing::randomWalk(400));
  const auto loaded = testing::loadProgram(kSmaCross);
  return runBacktest(spec, timeline, loaded.program, loaded.params, overrides, dsl::Level::Ok, nullptr);
}

TEST(Determinism, SameDigestTwice) {
  const RunOutput first = runSma({});
  const RunOutput second = runSma({});
  ASSERT_GT(first.fills.size(), 10u);  // прогон содержательный
  EXPECT_EQ(first.digest, second.digest);
  EXPECT_EQ(first.digest, digestOf(first));
  EXPECT_NE(first.digest, 0u);

  const RunOutput other = runSma({{"n", 12}});
  EXPECT_NE(first.digest, other.digest);
}

TEST(Determinism, DigestCoversFillsEquityAndStatus) {
  const RunOutput out = runSma({});
  const std::uint64_t base = digestOf(out);
  const Decimal kopeck = testing::dec("0.01");

  RunOutput changedFill = out;
  changedFill.fills.front().fee = changedFill.fills.front().fee + kopeck;
  EXPECT_NE(digestOf(changedFill), base);

  RunOutput changedEquity = out;
  changedEquity.equity.back().equity = changedEquity.equity.back().equity + kopeck;
  EXPECT_NE(digestOf(changedEquity), base);

  RunOutput changedStatus = out;
  changedStatus.status = RunStatus::Invalid;
  EXPECT_NE(digestOf(changedStatus), base);
}

}  // namespace
}  // namespace core
