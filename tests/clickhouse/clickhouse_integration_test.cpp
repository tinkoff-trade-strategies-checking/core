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
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Миллисекунды Unix UTC для момента по UTC.
std::int64_t utcMs(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
  return (daysFromCivil(y, static_cast<unsigned>(mo),
                        static_cast<unsigned>(d)) *
              86400 +
          h * 3600 + mi * 60 + s) *
         1000;
}

class ClickHouseIntegration : public ::testing::Test {
protected:
  void SetUp() override {
    const char *url = std::getenv("CLICKHOUSE_URL");
    if (url == nullptr || std::string(url).empty()) {
      GTEST_SKIP() << "CLICKHOUSE_URL не задан: интеграционный тест пропущен";
    }
    source_ = std::make_unique<worker::ClickHouseBarSource>(
        worker::ClickHouseConfig::fromUrl(url));
  }
  std::unique_ptr<worker::ClickHouseBarSource> source_;
};

TEST_F(ClickHouseIntegration, DailyCandlesFromStoredCandles) {
  const auto bars = source_->load(
      {kFixture, "SBER", 86400, utcMs(2026, 10, 1), utcMs(2026, 11, 30)});
  ASSERT_EQ(bars.size(), 10u); // 10 торговых дней 19-30.10.2026
  for (std::size_t i = 0; i < bars.size(); ++i) {
    EXPECT_EQ(bars[i].index, static_cast<std::int64_t>(i));
    if (i > 0) {
      EXPECT_LT(bars[i - 1].tsOpenMs, bars[i].tsOpenMs);
    }
    EXPECT_GE(bars[i].availableAtMs, bars[i].tsOpenMs);
  }
  // 2026-10-19 00:00 МСК = 2026-10-18 21:00 UTC; значения - из candles_1d.csv.
  const auto &first = bars.front();
  EXPECT_EQ(first.tsOpenMs, utcMs(2026, 10, 18, 21));
  EXPECT_EQ(first.open, Decimal::parse("287.49"));
  EXPECT_EQ(first.high, Decimal::parse("291.9"));
  EXPECT_EQ(first.low, Decimal::parse("286.42"));
  EXPECT_EQ(first.close, Decimal::parse("290.7"));
  EXPECT_EQ(first.volume, 36293649);
  EXPECT_EQ(first.value, Decimal::parse("10497642939.88"));
  // Дневная свеча доступна не раньше конца торгового дня: для дня без
  // внутридневных данных - 00:00 МСК следующих суток.
  EXPECT_EQ(first.availableAtMs, utcMs(2026, 10, 19, 21));
}

TEST_F(ClickHouseIntegration, MinuteCandlesAndHalfOpenRange) {
  // 2026-10-28 06:59 МСК = 03:59 UTC - первая минутная свеча SBER.
  const std::int64_t t0 = utcMs(2026, 10, 28, 3, 59);
  const auto one = source_->load({kFixture, "SBER", 60, t0, t0 + 60000});
  ASSERT_EQ(one.size(), 1u); // правая граница не включается
  EXPECT_EQ(one[0].tsOpenMs, t0);
  EXPECT_EQ(one[0].index, 0);
  EXPECT_EQ(one[0].open, Decimal::parse("298.2"));
  EXPECT_EQ(one[0].close, Decimal::parse("298.2"));
  EXPECT_EQ(one[0].volume, 4755);
  EXPECT_EQ(one[0].value, Decimal::parse("1417941"));
  EXPECT_EQ(one[0].availableAtMs, t0 + 60000);

  const auto two = source_->load({kFixture, "SBER", 60, t0, t0 + 120000});
  ASSERT_EQ(two.size(), 2u);
  EXPECT_EQ(two[1].index, 1);
  EXPECT_EQ(two[1].open, Decimal::parse("298.21"));
  EXPECT_EQ(two[1].close, Decimal::parse("298.58"));

  // Диапазон со сдвигом: нумерация снова с нуля.
  const auto shifted =
      source_->load({kFixture, "SBER", 60, t0 + 60000, t0 + 180000});
  ASSERT_EQ(shifted.size(), 2u);
  EXPECT_EQ(shifted[0].index, 0);
  EXPECT_EQ(shifted[0].tsOpenMs, t0 + 60000);
}

TEST_F(ClickHouseIntegration, BarNotYetAvailableByToMsIsExcluded) {
  // Дневная свеча 19.10 (открытие 18.10 21:00 UTC) доступна в 19.10 21:00 UTC
  // (INV-1).
  const std::int64_t from = utcMs(2026, 10, 18, 21);
  EXPECT_TRUE(
      source_->load({kFixture, "SBER", 86400, from, utcMs(2026, 10, 19, 12)})
          .empty());
  EXPECT_EQ(
      source_->load({kFixture, "SBER", 86400, from, utcMs(2026, 10, 19, 21)})
          .size(),
      1u);
}

TEST_F(ClickHouseIntegration, CandlesAggregatedFromTrades) {
  // SYNTH: только сделки; свечей в candles нет. Ожидания - из fixtures/README и
  // test_clickhouse.py.
  const auto bars = source_->load(
      {kFixture, "SYNTH", 60, utcMs(2026, 10, 1), utcMs(2026, 11, 30)});
  ASSERT_EQ(bars.size(), 6u);
  // 10:00 МСК = 07:00 UTC
  EXPECT_EQ(bars[0].tsOpenMs, utcMs(2026, 10, 28, 7, 0));
  EXPECT_EQ(bars[0].open, Decimal::parse("100"));
  EXPECT_EQ(bars[0].high, Decimal::parse("101.5"));
  EXPECT_EQ(bars[0].low, Decimal::parse("99.5"));
  EXPECT_EQ(bars[0].close, Decimal::parse("99.5"));
  EXPECT_EQ(bars[0].volume, 42);
  EXPECT_EQ(bars[0].value, Decimal::parse("4224"));
  EXPECT_EQ(bars[0].availableAtMs, utcMs(2026, 10, 28, 7, 1)); // закрытие свечи
  // Две сделки в одну микросекунду: close по большему trade_no.
  EXPECT_EQ(bars[1].open, Decimal::parse("100"));
  EXPECT_EQ(bars[1].high, Decimal::parse("102"));
  EXPECT_EQ(bars[1].low, Decimal::parse("98"));
  EXPECT_EQ(bars[1].close, Decimal::parse("98"));
  EXPECT_EQ(bars[1].volume, 13);
  for (std::size_t i = 0; i < bars.size(); ++i)
    EXPECT_EQ(bars[i].index, static_cast<std::int64_t>(i));

  // Сутки по Москве: сделки 23:59:59.999999 и 00:00:00 МСК попадают в разные
  // дни.
  const auto days = source_->load(
      {kFixture, "SYNTH", 86400, utcMs(2026, 10, 1), utcMs(2026, 11, 30)});
  ASSERT_EQ(days.size(), 2u);
  EXPECT_EQ(days[0].tsOpenMs, utcMs(2026, 10, 27, 21));
  EXPECT_EQ(days[0].volume, 65);
  EXPECT_EQ(days[1].volume, 10);
}

TEST_F(ClickHouseIntegration, LoadIsDeterministic) {
  const core::BarRequest req{kFixture, "GAZP", 60, utcMs(2026, 10, 28),
                             utcMs(2026, 10, 30)};
  const auto a = source_->load(req);
  const auto b = source_->load(req);
  ASSERT_FALSE(a.empty());
  EXPECT_EQ(a, b);
}

TEST_F(ClickHouseIntegration, Security) {
  const auto sber = source_->security(kFixture, "SBER");
  EXPECT_EQ(sber.secid, "SBER");
  EXPECT_EQ(sber.sector, "Финансы");
  EXPECT_EQ(sber.lotSize, 1);
  EXPECT_EQ(sber.priceStep, Decimal::parse("0.01"));
  const auto gazp = source_->security(kFixture, "GAZP");
  EXPECT_EQ(gazp.lotSize, 10);
  EXPECT_EQ(source_->security(kFixture, "LKOH").priceStep,
            Decimal::parse("0.5"));
}

TEST_F(ClickHouseIntegration, UnknownSnapshotAndSecurity) {
  EXPECT_THROW(source_->load({"00000000-0000-4000-8000-000000000000", "SBER",
                              60, 0, 1000}),
               core::SnapshotNotFound);
  EXPECT_THROW(
      source_->security("00000000-0000-4000-8000-000000000000", "SBER"),
      core::SnapshotNotFound);
  EXPECT_THROW(source_->load({kFixture, "NOSUCH", 60, 0, 1000}),
               core::SecurityNotFound);
  EXPECT_THROW(source_->security(kFixture, "NOSUCH"), core::SecurityNotFound);
}

TEST_F(ClickHouseIntegration, EmptyRangeIsNotAnError) {
  EXPECT_TRUE(
      source_
          ->load({kFixture, "SBER", 60, utcMs(2020, 1, 1), utcMs(2020, 1, 2)})
          .empty());
}

} // namespace
