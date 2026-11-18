// Интеграционный тест ClickHouseBarSource на настоящем ClickHouse с
// фикстурой-снапшотом. Пропускается, если не задана переменная окружения
// CLICKHOUSE_URL (формат http://user:password@host:8123/ttsc). Как запустить -
// в core/AGENTS.md.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#include "clickhouse/clickhouse_bar_source.hpp"

namespace {

using core::Decimal;

// Фикстура marketdata-import: фиксированный uuid5.
constexpr const char *kFixture = "ec6b3c97-7036-56cf-ba4d-ec4e919e682d";

// Дни от 1970-01-01 до гражданской даты (алгоритм Говарда Хиннанта), чистая
// арифметика без часов.
std::int64_t daysFromCivil(std::int64_t Y, unsigned M, unsigned D) {
  Y -= M <= 2;
  const std::int64_t Era = (Y >= 0 ? Y : Y - 399) / 400;
  const unsigned Yoe = static_cast<unsigned>(Y - Era * 400);
  const unsigned Doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
  const unsigned Doe = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;
  return Era * 146097 + static_cast<std::int64_t>(Doe) - 719468;
}

// Миллисекунды Unix UTC для момента по UTC.
std::int64_t utcMs(int Y, int Mo, int D, int H = 0, int Mi = 0, int S = 0) {
  return (daysFromCivil(Y, static_cast<unsigned>(Mo),
                        static_cast<unsigned>(D)) *
              86400 +
          H * 3600 + Mi * 60 + S) *
         1000;
}

class ClickHouseIntegration : public ::testing::Test {
protected:
  void SetUp() override {
    const char *Url = std::getenv("CLICKHOUSE_URL");
    if (Url == nullptr || std::string(Url).empty()) {
      GTEST_SKIP() << "CLICKHOUSE_URL не задан: интеграционный тест пропущен";
    }
    Source = std::make_unique<worker::ClickHouseBarSource>(
        worker::ClickHouseConfig::fromUrl(Url));
  }
  std::unique_ptr<worker::ClickHouseBarSource> Source;
};

TEST_F(ClickHouseIntegration, DailyCandlesFromStoredCandles) {
  const auto Bars = Source->load(
      {kFixture, "SBER", 86400, utcMs(2026, 10, 1), utcMs(2026, 11, 30)});
  ASSERT_EQ(Bars.size(), 10u); // 10 торговых дней 19-30.10.2026
  for (std::size_t I = 0; I < Bars.size(); ++I) {
    EXPECT_EQ(Bars[I].Index, static_cast<std::int64_t>(I));
    if (I > 0) {
      EXPECT_LT(Bars[I - 1].TsOpenMs, Bars[I].TsOpenMs);
    }
    EXPECT_GE(Bars[I].AvailableAtMs, Bars[I].TsOpenMs);
  }
  // 2026-10-19 00:00 МСК = 2026-10-18 21:00 UTC; значения - из candles_1d.csv.
  const auto &First = Bars.front();
  EXPECT_EQ(First.TsOpenMs, utcMs(2026, 10, 18, 21));
  EXPECT_EQ(First.Open, Decimal::parse("287.49"));
  EXPECT_EQ(First.High, Decimal::parse("291.9"));
  EXPECT_EQ(First.Low, Decimal::parse("286.42"));
  EXPECT_EQ(First.Close, Decimal::parse("290.7"));
  EXPECT_EQ(First.Volume, 36293649);
  EXPECT_EQ(First.Value, Decimal::parse("10497642939.88"));
  // Дневная свеча доступна не раньше конца торгового дня: для дня без
  // внутридневных данных - 00:00 МСК следующих суток.
  EXPECT_EQ(First.AvailableAtMs, utcMs(2026, 10, 19, 21));
}

TEST_F(ClickHouseIntegration, MinuteCandlesAndHalfOpenRange) {
  // 2026-10-28 06:59 МСК = 03:59 UTC - первая минутная свеча SBER.
  const std::int64_t T0 = utcMs(2026, 10, 28, 3, 59);
  const auto One = Source->load({kFixture, "SBER", 60, T0, T0 + 60000});
  ASSERT_EQ(One.size(), 1u); // правая граница не включается
  EXPECT_EQ(One[0].TsOpenMs, T0);
  EXPECT_EQ(One[0].Index, 0);
  EXPECT_EQ(One[0].Open, Decimal::parse("298.2"));
  EXPECT_EQ(One[0].Close, Decimal::parse("298.2"));
  EXPECT_EQ(One[0].Volume, 4755);
  EXPECT_EQ(One[0].Value, Decimal::parse("1417941"));
  EXPECT_EQ(One[0].AvailableAtMs, T0 + 60000);

  const auto Two = Source->load({kFixture, "SBER", 60, T0, T0 + 120000});
  ASSERT_EQ(Two.size(), 2u);
  EXPECT_EQ(Two[1].Index, 1);
  EXPECT_EQ(Two[1].Open, Decimal::parse("298.21"));
  EXPECT_EQ(Two[1].Close, Decimal::parse("298.58"));

  // Диапазон со сдвигом: нумерация снова с нуля.
  const auto Shifted =
      Source->load({kFixture, "SBER", 60, T0 + 60000, T0 + 180000});
  ASSERT_EQ(Shifted.size(), 2u);
  EXPECT_EQ(Shifted[0].Index, 0);
  EXPECT_EQ(Shifted[0].TsOpenMs, T0 + 60000);
}

TEST_F(ClickHouseIntegration, BarNotYetAvailableByToMsIsExcluded) {
  // Дневная свеча 19.10 (открытие 18.10 21:00 UTC) доступна в 19.10 21:00 UTC
  // (INV-1).
  const std::int64_t From = utcMs(2026, 10, 18, 21);
  EXPECT_TRUE(
      Source->load({kFixture, "SBER", 86400, From, utcMs(2026, 10, 19, 12)})
          .empty());
  EXPECT_EQ(
      Source->load({kFixture, "SBER", 86400, From, utcMs(2026, 10, 19, 21)})
          .size(),
      1u);
}

TEST_F(ClickHouseIntegration, CandlesAggregatedFromMinutes) {
  // SBER: 5-минутных свечей в таблице candles нет, они агрегируются из минутных.
  const std::int64_t T0 = utcMs(2026, 10, 28, 3, 59);
  const auto FiveMin = Source->load({kFixture, "SBER", 300, T0, T0 + 600000});
  ASSERT_FALSE(FiveMin.empty());
  for (std::size_t I = 0; I < FiveMin.size(); ++I) {
    EXPECT_EQ(FiveMin[I].Index, static_cast<std::int64_t>(I));
    EXPECT_GE(FiveMin[I].AvailableAtMs, FiveMin[I].TsOpenMs);
    if (I > 0) {
      EXPECT_LT(FiveMin[I - 1].TsOpenMs, FiveMin[I].TsOpenMs);
    }
  }
}

TEST_F(ClickHouseIntegration, LoadIsDeterministic) {
  const core::BarRequest Req{kFixture, "GAZP", 60, utcMs(2026, 10, 28),
                             utcMs(2026, 10, 30)};
  const auto A = Source->load(Req);
  const auto B = Source->load(Req);
  ASSERT_FALSE(A.empty());
  EXPECT_EQ(A, B);
}

TEST_F(ClickHouseIntegration, Security) {
  const auto Sber = Source->security(kFixture, "SBER");
  EXPECT_EQ(Sber.Secid, "SBER");
  EXPECT_EQ(Sber.Sector, "Финансы");
  EXPECT_EQ(Sber.LotSize, 1);
  EXPECT_EQ(Sber.PriceStep, Decimal::parse("0.01"));
  const auto Gazp = Source->security(kFixture, "GAZP");
  EXPECT_EQ(Gazp.LotSize, 10);
  EXPECT_EQ(Source->security(kFixture, "LKOH").PriceStep,
            Decimal::parse("0.5"));
}

TEST_F(ClickHouseIntegration, UnknownSnapshotAndSecurity) {
  EXPECT_THROW(Source->load({"00000000-0000-4000-8000-000000000000", "SBER", 60,
                             0, 1000}),
               core::SnapshotNotFound);
  EXPECT_THROW(Source->security("00000000-0000-4000-8000-000000000000", "SBER"),
               core::SnapshotNotFound);
  EXPECT_THROW(Source->load({kFixture, "NOSUCH", 60, 0, 1000}),
               core::SecurityNotFound);
  EXPECT_THROW(Source->security(kFixture, "NOSUCH"), core::SecurityNotFound);
}

TEST_F(ClickHouseIntegration, EmptyRangeIsNotAnError) {
  EXPECT_TRUE(
      Source->load({kFixture, "SBER", 60, utcMs(2020, 1, 1), utcMs(2020, 1, 2)})
          .empty());
}

} // namespace
