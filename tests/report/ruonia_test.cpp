#include "core/report/ruonia.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <stdexcept>
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
      DailyRate{20240105, d("14.5")},
      DailyRate{20240108, d("14.5")},
      DailyRate{20240110, d("15.0")},
      DailyRate{20240112, d("15.2")},
  };

  const std::vector<std::int32_t> Dates = {
      20240108, // пн: нет предыдущего дня, ставка <= 07.01 (14.5), Delta = 1
      20240109, // вт: RUONIA_{d-1}=14.5 (от 20240108), Delta = 1
      20240110, // ср: RUONIA_{d-1}=14.5 (от 20240109), Delta = 1
      20240111, // чт: RUONIA_{d-1}=15.0 (от 20240110), Delta = 1
      20240112, // пт: RUONIA_{d-1}=15.0 (от 20240111), Delta = 1
      20240115, // пн: RUONIA_{d-1}=15.2 (от 20240112), Delta = 3
  };

  const MatchedRuonia M = matchRuoniaRates(Series, Dates);
  ASSERT_EQ(M.Rates.size(), 6u);
  ASSERT_EQ(M.CalendarDays.size(), 6u);

  EXPECT_EQ(M.Rates[0], d("14.5"));
  EXPECT_EQ(M.Rates[1], d("14.5"));
  EXPECT_EQ(M.Rates[2], d("14.5"));
  EXPECT_EQ(M.Rates[3], d("15.0"));
  EXPECT_EQ(M.Rates[4], d("15.0"));
  EXPECT_EQ(M.Rates[5], d("15.2"));

  EXPECT_EQ(M.CalendarDays[0], 1);
  EXPECT_EQ(M.CalendarDays[1], 1);
  EXPECT_EQ(M.CalendarDays[2], 1);
  EXPECT_EQ(M.CalendarDays[3], 1);
  EXPECT_EQ(M.CalendarDays[4], 1);
  EXPECT_EQ(M.CalendarDays[5], 3);
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
  const MatchedRuonia M = matchRuoniaRates(Series, {});
  EXPECT_TRUE(M.Rates.empty());
  EXPECT_TRUE(M.CalendarDays.empty());
}

TEST(Ruonia, MatchWithTimeline) {
  auto F = testing::fixture1();
  const Timeline T =
      Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                     Timeframe::D1, 20240109, 20240122);
  ASSERT_EQ(T.totalSteps(), 10);

  RuoniaSeries Series;
  Series.Rates = {
      DailyRate{20240108, d("15.0")},
      DailyRate{20240115, d("16.0")},
  };

  // Первые 4 дня (шаги 0…3: 2024-01-09, 10, 11, 12 - вторник..пятница)
  const MatchedRuonia M4 = matchRuoniaRates(Series, T, 3);
  ASSERT_EQ(M4.Rates.size(), 4u);
  ASSERT_EQ(M4.CalendarDays.size(), 4u);
  EXPECT_EQ(M4.Rates[0], d("15.0"));
  EXPECT_EQ(M4.Rates[1], d("15.0"));
  EXPECT_EQ(M4.Rates[2], d("15.0"));
  EXPECT_EQ(M4.Rates[3], d("15.0"));
  EXPECT_EQ(M4.CalendarDays[0], 1);
  EXPECT_EQ(M4.CalendarDays[1], 1);
  EXPECT_EQ(M4.CalendarDays[2], 1);
  EXPECT_EQ(M4.CalendarDays[3], 1);

  // Все 10 дней
  const MatchedRuonia M10 = matchRuoniaRates(Series, T, 9);
  ASSERT_EQ(M10.Rates.size(), 10u);
  ASSERT_EQ(M10.CalendarDays.size(), 10u);
  EXPECT_EQ(M10.Rates[3], d("15.0"));
  EXPECT_EQ(M10.Rates[4],
            d("15.0")); // 2024-01-15 берёт пятничную ставку (15.0)
  EXPECT_EQ(M10.Rates[5],
            d("16.0")); // 2024-01-16 берёт понедельничную ставку (16.0)
  EXPECT_EQ(M10.Rates[9], d("16.0")); // forward fill до 2024-01-22

  EXPECT_EQ(M10.CalendarDays[4], 3); // пятница 12 -> понедельник 15
  EXPECT_EQ(M10.CalendarDays[9], 3); // пятница 19 -> понедельник 22
}

TEST(Ruonia, FullYearConstantRateSumsToAnnualRate) {
  // 52 недели по 5 рабочих дней (пн-пт) = 260 рабочих дней.
  // За год постоянная ставка 15% годовых по формуле:
  // r_{f,d} = Rate / 100 * Delta_d / 365
  // должна суммироваться в полную ставку (0.15 * TotalDays / 365 ~ 0.15).
  RuoniaSeries Series;
  Series.Rates = {DailyRate{20230106, d("15.0")}};

  std::vector<std::int32_t> Dates;
  auto Day = std::chrono::year{2023} / std::chrono::January / 9;
  std::chrono::sys_days SysDay{Day};

  for (int W = 0; W < 52; ++W) {
    for (int D = 0; D < 5; ++D) {
      const std::chrono::year_month_day Ymd{SysDay};
      const std::int32_t YmdInt = static_cast<int>(Ymd.year()) * 10000 +
                                  static_cast<unsigned>(Ymd.month()) * 100 +
                                  static_cast<unsigned>(Ymd.day());
      Dates.push_back(YmdInt);
      SysDay += std::chrono::days{1};
    }
    // Выходные сб, вс
    SysDay += std::chrono::days{2};
  }

  ASSERT_EQ(Dates.size(), 260u);
  // Предыдущий торговый день перед периодом - пятница 2023-01-06.
  const MatchedRuonia M = matchRuoniaRates(Series, Dates, 20230106);
  ASSERT_EQ(M.Rates.size(), 260u);
  ASSERT_EQ(M.CalendarDays.size(), 260u);

  std::int32_t TotalDays = 0;
  double SumRf = 0;
  for (std::size_t I = 0; I < M.Rates.size(); ++I) {
    TotalDays += M.CalendarDays[I];
    SumRf += (M.Rates[I].toDouble() / 100.0) * (M.CalendarDays[I] / 365.0);
  }

  // Сумма Delta - календарные дни от предыдущего торгового дня до конца
  // периода.
  EXPECT_EQ(TotalDays, calendarDaysBetween(20230106, Dates.back()));
  EXPECT_EQ(TotalDays, 364);
  // Сумма r_f за год совпадает с годовой ставкой 15% с погрешностью неполного
  // 365-го дня
  EXPECT_NEAR(SumRf, 0.15 * (static_cast<double>(TotalDays) / 365.0), 1e-9);
  EXPECT_NEAR(SumRf, 0.15, 0.003);
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
    In.CalendarDays.push_back(1);
  }

  const Report R = computeReport(In);
  const Metrics &M = R.Stats;

  EXPECT_NEAR(M.TotalReturn, 0.03, kTol);
  ASSERT_TRUE(M.Volatility);
  // Волатильность считается по доходностям портфеля (без RUONIA)
  EXPECT_NEAR(*M.Volatility, 0.4088278812, kTol);

  // Sharpe и Sortino с безрисковой ставкой Actual/365 (по умолчанию Delta_d =
  // 1 в CalendarDays):
  ASSERT_TRUE(M.Sharpe);
  EXPECT_NEAR(*M.Sharpe, 3.5642943496, kTol);
  ASSERT_TRUE(M.Sortino);
  EXPECT_NEAR(*M.Sortino, 6.7739866778, kTol);
}

TEST(Ruonia, SharpeAndSortinoWithCalendarDays) {
  MetricsInput In;
  In.InitialCash = d("1000000");
  const std::vector<const char *> Equities = {"1010000", "1005000", "1020000",
                                              "990000", "1030000"};
  for (std::size_t I = 0; I < Equities.size(); ++I) {
    In.Equity.push_back({static_cast<std::int64_t>(I), d(Equities[I])});
    In.TradingDayOfStep.push_back(static_cast<std::int32_t>(I));
    In.RuoniaRates.push_back(d("15.0")); // 15% годовых
  }
  // 5 дней с переходом через выходные (пт -> пн = 3 дня)
  In.CalendarDays = {1, 1, 1, 1, 3};

  const Report R = computeReport(In);
  const Metrics &M = R.Stats;

  EXPECT_NEAR(M.TotalReturn, 0.03, kTol);
  ASSERT_TRUE(M.Sharpe);
  // При начислении за выходные Sharpe немного ниже (больше r_f вычитается из
  // r_d)
  EXPECT_NEAR(*M.Sharpe, 3.4999013891, kTol);
  ASSERT_TRUE(M.Sortino);
  EXPECT_NEAR(*M.Sortino, 6.5814164267, kTol);
}

TEST(Ruonia, FirstDayMondayUsesPreviousFridayRateAndDelta3) {
  RuoniaSeries Series;
  Series.Rates = {DailyRate{20240112, d("15.0")},
                  DailyRate{20240115, d("16.0")}};
  const MatchedRuonia M =
      matchRuoniaRates(Series, {20240115, 20240116}, 20240112);
  ASSERT_EQ(M.Rates.size(), 2u);
  EXPECT_EQ(M.Rates[0], d("15.0")); // не ставка дня d (16.0)
  EXPECT_EQ(M.CalendarDays[0], 3);
  EXPECT_EQ(M.Rates[1], d("16.0"));
  EXPECT_EQ(M.CalendarDays[1], 1);
}

TEST(Ruonia, FirstDayWithoutPreviousDayUsesRateBeforeDayWithDelta1) {
  RuoniaSeries Series;
  Series.Rates = {DailyRate{20240112, d("15.0")},
                  DailyRate{20240115, d("16.0")}};
  const MatchedRuonia M = matchRuoniaRates(Series, {20240115});
  EXPECT_EQ(M.Rates[0], d("15.0"));
  EXPECT_EQ(M.CalendarDays[0], 1);

  // Ставка есть только на день d - ошибка данных, не подстановка ставки дня d.
  RuoniaSeries OnlyToday;
  OnlyToday.Rates = {DailyRate{20240115, d("16.0")}};
  EXPECT_THROW((void)matchRuoniaRates(OnlyToday, {20240115}), MarketDataError);
}

TEST(Ruonia, TimelineWithWarmupUsesLastWarmupDay) {
  auto F = testing::fixture1();
  // С 15 января (понедельник); прогрев - 12 января (пятница).
  const Timeline T =
      Timeline::loadWithWarmup(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                               Timeframe::D1, 20240115, 20240122, 1);
  ASSERT_EQ(T.previousDayYmd(), 20240112);
  RuoniaSeries Series;
  Series.Rates = {DailyRate{20240112, d("15.0")},
                  DailyRate{20240115, d("16.0")}};
  const MatchedRuonia M = matchRuoniaRates(Series, T, 0);
  ASSERT_EQ(M.Rates.size(), 1u);
  EXPECT_EQ(M.Rates[0], d("15.0"));
  EXPECT_EQ(M.CalendarDays[0], 3);
}

TEST(Ruonia, CalendarDaysSizeMismatchThrows) {
  MetricsInput In;
  In.InitialCash = d("1000000");
  In.Equity = {{0, d("1010000")}, {1, d("1020000")}};
  In.TradingDayOfStep = {0, 1};
  In.RuoniaRates = {d("15.0"), d("15.0")};
  In.CalendarDays = {1};
  EXPECT_THROW((void)computeReport(In), MarketDataError);
}

TEST(Ruonia, PreviousDayNotBeforeFirstDayThrows) {
  RuoniaSeries Series;
  Series.Rates = {DailyRate{20240112, d("15.0")}};
  EXPECT_THROW((void)matchRuoniaRates(Series, {20240115}, 20240115),
               std::invalid_argument);
  EXPECT_THROW((void)matchRuoniaRates(Series, {20240115}, 20240116),
               std::invalid_argument);
}

TEST(Ruonia, AddCalendarDaysCrossesMonthAndYear) {
  EXPECT_EQ(addCalendarDays(20240301, -1), 20240229);
  EXPECT_EQ(addCalendarDays(20240101, -1), 20231231);
  EXPECT_EQ(addCalendarDays(20231231, 1), 20240101);
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
