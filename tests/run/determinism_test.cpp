// Детерминизм (INV-4): один и тот же вход даёт побитово тот же результат
// прогона.
#include <gtest/gtest.h>

#include <cstdint>

#include "core/run/backtest.hpp"
#include "run/run_test_support.hpp"

namespace core {
namespace {

constexpr const char *kSmaCross =
    "param n = 5 range 2..20 step 1\n"
    "rule \"b\" when close > sma(close, n) and position == 0 then buy 10 lots "
    "because \"выше средней {n}\"\n"
    "rule \"s\" when close < sma(close, n) and position > 0 then sell all "
    "because \"ниже средней {n}\"\n";

RunOutput runSma(const dsl::ParamValues &Overrides) {
  const auto [spec, timeline] =
      testing::syntheticTimeline(testing::randomWalk(400));
  const auto Loaded = testing::loadProgram(kSmaCross);
  return runBacktest(spec, timeline, Loaded.Program, Loaded.Params, Overrides,
                     dsl::Level::Ok, nullptr);
}

TEST(Determinism, SameDigestTwice) {
  const RunOutput First = runSma({});
  const RunOutput Second = runSma({});
  ASSERT_GT(First.Fills.size(), 10u); // прогон содержательный
  EXPECT_EQ(First.Digest, Second.Digest);
  EXPECT_EQ(First.Digest, digestOf(First));
  EXPECT_NE(First.Digest, 0u);

  const RunOutput Other = runSma({{"n", 12}});
  EXPECT_NE(First.Digest, Other.Digest);
}

TEST(Determinism, DigestCoversFillsEquityAndStatus) {
  const RunOutput Out = runSma({});
  const std::uint64_t Base = digestOf(Out);
  const Decimal Kopeck = testing::dec("0.01");

  RunOutput ChangedFill = Out;
  ChangedFill.Fills.front().Fee = ChangedFill.Fills.front().Fee + Kopeck;
  EXPECT_NE(digestOf(ChangedFill), Base);

  RunOutput ChangedEquity = Out;
  ChangedEquity.Equity.back().Equity =
      ChangedEquity.Equity.back().Equity + Kopeck;
  EXPECT_NE(digestOf(ChangedEquity), Base);

  RunOutput ChangedStatus = Out;
  ChangedStatus.Status = RunStatus::Invalid;
  EXPECT_NE(digestOf(ChangedStatus), Base);
}

} // namespace
} // namespace core
