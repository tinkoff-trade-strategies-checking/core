#include "core/report/ruonia.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "core/market/bar_source.hpp" // MarketDataError
#include "core/report/metrics.hpp"
#include "support/fixture1.hpp"

namespace core {
namespace {

constexpr double kTol = 1e-9;

Decimal d(const char *Text) { return Decimal::parse(Text); }

TEST(Ruonia, MatchExactAndForwardFill) {
  RuoniaSeries Series;
  Series.Rates = {
      DailyRate{20240108, d("14.5")},
      DailyRate{20240110, d("15.0")},
      DailyRate{20240112, d("15.2")},
  };

  const std::vector<std::int32_t> Dates = {
      20240108, // точное 14.5
      20240109, // forward fill от 20240108 -> 14.5
      20240110, // точное 15.0
      20240111, // forward fill от 20240110 -> 15.0
      20240112, // точное 15.2
      20240115, // forward fill через выходные от 20240112 -> 15.2
  };

  const std::vector<Decimal> Rates = matchRuoniaRates(Series, Dates);
  ASSERT_EQ(Rates.size(), 6u);
  EXPECT_EQ(Rates[0], d("14.5"));
  EXPECT_EQ(Rates[1], d("14.5"));
  EXPECT_EQ(Rates[2], d("15.0"));
  EXPECT_EQ(Rates[3], d("15.0"));
  EXPECT_EQ(Rates[4], d("15.2"));
  EXPECT_EQ(Rates[5], d("15.2"));
}

TEST(Ruonia, MissingSeriesThrowsMarketDataError) {
  RuoniaSeries Empty;
  EXPECT_THROW((void)matchRuoniaRates(Empty, {20240109}), MarketDataError);
}

TEST(Ruonia, MissingBeforeFirstDayThrowsMarketDataError) {
  RuoniaSeries Series;
  Series.Rates = {DailyRate{20240110, d("15.0")}};
  // Дата 20240109 раньше первой доступной ставки (20240110)
  EXPECT_THROW((void)matchRuoniaRates(Series, {20240109, 20240110}),
               MarketDataError);
}

TEST(Ruonia, EmptyDatesReturnsEmpty) {
  RuoniaSeries Series;
  Series.Rates = {DailyRate{20240110, d("15.0")}};
  EXPECT_TRUE(matchRuoniaRates(Series, {}).empty());
}

TEST(Ruonia, MatchWithTimeline) {
  auto F = testing::fixture1();
  const Timeline T =
      Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                     Timeframe::D1, 20240109, 20240122);
  ASSERT_EQ(T.totalSteps(), 10);

  RuoniaSeries Series;
  Series.Rates = {
      DailyRate{20240109, d("15.0")},
      DailyRate{20240115, d("16.0")},
  };

  // Первые 4 дня (шаги 0…3: 2024-01-09, 10, 11, 12)
  const std::vector<Decimal> Rates4 = matchRuoniaRates(Series, T, 3);
  ASSERT_EQ(Rates4.size(), 4u);
  EXPECT_EQ(Rates4[0], d("15.0"));
  EXPECT_EQ(Rates4[1], d("15.0")); // forward fill
  EXPECT_EQ(Rates4[2], d("15.0")); // forward fill
  EXPECT_EQ(Rates4[3], d("15.0")); // forward fill

  // Все 10 дней
  const std::vector<Decimal> Rates10 = matchRuoniaRates(Series, T, 9);
  ASSERT_EQ(Rates10.size(), 10u);
  EXPECT_EQ(Rates10[3], d("15.0"));
  EXPECT_EQ(Rates10[4], d("16.0")); // 2024-01-15
  EXPECT_EQ(Rates10[9], d("16.0")); // forward fill до 2024-01-22
}

TEST(Ruonia, SharpeAndSortinoWithKnownRate) {
  MetricsInput In;
  In.InitialCash = d("1000000");
  const std::vector<const char *> Equities = {"1010000", "1005000", "1020000",
                                              "990000", "1030000"};
  for (std::size_t I = 0; I < Equities.size(); ++I) {
    In.Equity.push_back({static_cast<std::int64_t>(I), d(Equities[I])});
    In.TradingDayOfStep.push_back(static_cast<std::int32_t>(I));
    In.RuoniaRates.push_back(d("15.0")); // 15% годовых
  }

  const Report R = computeReport(In);
  const Metrics &M = R.Stats;

  EXPECT_NEAR(M.TotalReturn, 0.03, kTol);
  ASSERT_TRUE(M.Volatility);
  // Волатильность считается по доходностям портфеля (без RUONIA)
  EXPECT_NEAR(*M.Volatility, 0.4088278812, kTol);

  // Sharpe и Sortino с безрисковой ставкой Actual/365:
  ASSERT_TRUE(M.Sharpe);
  EXPECT_NEAR(*M.Sharpe, 3.5642943496, kTol);
  ASSERT_TRUE(M.Sortino);
  EXPECT_NEAR(*M.Sortino, 6.7739866778, kTol);
}

TEST(Ruonia, MissingRuoniaAtTradingDaysGe2Throws) {
  MetricsInput In;
  In.InitialCash = d("1000000");
  In.Equity = {{0, d("1010000")}, {1, d("1020000")}};
  In.TradingDayOfStep = {0, 1};
  // RuoniaRates не задан при 2 торговых днях -> MarketDataError
  EXPECT_THROW((void)computeReport(In), MarketDataError);
}

} // namespace
} // namespace core
