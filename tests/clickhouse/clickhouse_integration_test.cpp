// Интеграционные тесты ClickHouseBarSource, ClickHouseCalendarSource и
// ClickHouseRuoniaSource на настоящем ClickHouse с фикстурой-снапшотом.
// Пропускаются, если не задана переменная окружения CLICKHOUSE_URL (формат
// http://user:password@host:8123/ttsc); при CLICKHOUSE_REQUIRED=1 (CI)
// отсутствие URL - падение. Как запустить - в core/AGENTS.md.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#include "clickhouse/clickhouse_bar_source.hpp"
#include "clickhouse/clickhouse_calendar_source.hpp"
#include "clickhouse/clickhouse_ruonia_source.hpp"

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
  // Без CLICKHOUSE_URL тест пропускается; в CI (CLICKHOUSE_REQUIRED=1) это
  // ошибка, чтобы тесты не пропускались молча.
  void SetUp() override {
    const char *Url = std::getenv("CLICKHOUSE_URL");
    if (Url == nullptr || *Url == '\0') {
      const char *Required = std::getenv("CLICKHOUSE_REQUIRED");
      if (Required != nullptr && std::string(Required) == "1")
        FAIL() << "CLICKHOUSE_REQUIRED=1, но CLICKHOUSE_URL не задан";
      GTEST_SKIP() << "CLICKHOUSE_URL не задан: интеграционный тест пропущен";
    }
    const auto Config = worker::ClickHouseConfig::fromUrl(Url);
    Source = std::make_unique<worker::ClickHouseBarSource>(Config);
    Calendar = std::make_unique<worker::ClickHouseCalendarSource>(Config);
    Ruonia = std::make_unique<worker::ClickHouseRuoniaSource>(Config);
  }
  std::unique_ptr<worker::ClickHouseBarSource> Source;
  std::unique_ptr<worker::ClickHouseCalendarSource> Calendar;
  std::unique_ptr<worker::ClickHouseRuoniaSource> Ruonia;
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
  // SBER: 5-минутных свечей в таблице candles нет, они агрегируются из
  // минутных. Выравнивание по 5 мин UTC: бар 04:00 - свечи 04:00…04:04, бар
  // 04:05 - свечи 04:05…04:08 (04:09 в данных нет). Бар 03:55 (одна свеча
  // 03:59) начинается раньше fromMs и не отдаётся.
  const std::int64_t T0 = utcMs(2026, 10, 28, 3, 59);
  const std::int64_t T4 = utcMs(2026, 10, 28, 4, 0);
  const std::int64_t T5 = utcMs(2026, 10, 28, 4, 5);
  const auto FiveMin = Source->load({kFixture, "SBER", 300, T0, T0 + 660000});
  ASSERT_EQ(FiveMin.size(), 2u);

  EXPECT_EQ(FiveMin[0].Index, 0);
  EXPECT_EQ(FiveMin[0].TsOpenMs, T4);
  EXPECT_EQ(FiveMin[0].Open, Decimal::parse("298.21"));
  EXPECT_EQ(FiveMin[0].High, Decimal::parse("298.9"));
  EXPECT_EQ(FiveMin[0].Low, Decimal::parse("298.21"));
  EXPECT_EQ(FiveMin[0].Close, Decimal::parse("298.85"));
  EXPECT_EQ(FiveMin[0].Volume, 20500);
  EXPECT_EQ(FiveMin[0].Value, Decimal::parse("6119000"));
  EXPECT_EQ(FiveMin[0].AvailableAtMs, T4 + 300000); // T + Δ

  EXPECT_EQ(FiveMin[1].Index, 1);
  EXPECT_EQ(FiveMin[1].TsOpenMs, T5);
  EXPECT_EQ(FiveMin[1].Open, Decimal::parse("298.85"));
  EXPECT_EQ(FiveMin[1].High, Decimal::parse("299.3"));
  EXPECT_EQ(FiveMin[1].Low, Decimal::parse("298.8"));
  EXPECT_EQ(FiveMin[1].Close, Decimal::parse("299.25"));
  EXPECT_EQ(FiveMin[1].Volume, 16800);
  EXPECT_EQ(FiveMin[1].Value, Decimal::parse("5023000"));
  EXPECT_EQ(FiveMin[1].AvailableAtMs, T5 + 300000);
}

TEST_F(ClickHouseIntegration, AggregatedBarIsAvailableAtIntervalEnd) {
  // Бар 04:00-04:05 доступен ровно в T + Δ = 04:05, не раньше.
  const std::int64_t T0 = utcMs(2026, 10, 28, 4, 0);
  EXPECT_TRUE(
      Source->load({kFixture, "SBER", 300, T0, utcMs(2026, 10, 28, 4, 4, 59)})
          .empty());
  EXPECT_EQ(Source->load({kFixture, "SBER", 300, T0, utcMs(2026, 10, 28, 4, 5)})
                .size(),
            1u);
  // Неполный интервал: последняя свеча SBER - 04:08 (известна в 04:09), бар
  // 04:05-04:10 по минуткам был бы доступен в 04:09, но доступен только в
  // T + Δ = 04:10 (INV-1, решение владельца продукта 18.11.2026).
  EXPECT_EQ(
      Source->load({kFixture, "SBER", 300, T0, utcMs(2026, 10, 28, 4, 9, 59)})
          .size(),
      1u);
  EXPECT_EQ(
      Source->load({kFixture, "SBER", 300, T0, utcMs(2026, 10, 28, 4, 10)})
          .size(),
      2u);

  // GAZP: свечи 07:00 и 07:01, в последние минуты интервала 07:00-07:05
  // сделок нет - бар всё равно доступен только в 07:05.
  const std::int64_t G0 = utcMs(2026, 10, 28, 7, 0);
  EXPECT_TRUE(Source->load({kFixture, "GAZP", 300, G0, G0 + 299000}).empty());
  const auto Gazp = Source->load({kFixture, "GAZP", 300, G0, G0 + 300000});
  ASSERT_EQ(Gazp.size(), 1u);
  EXPECT_EQ(Gazp[0].AvailableAtMs, G0 + 300000);
}

TEST_F(ClickHouseIntegration, DailyAggregationFromMinutes) {
  // DAYT: дневных свечей нет, дни МСК собираются из минутных
  // (kAggregateDailySql на настоящем ClickHouse). День 28.10 МСК
  // открывается 27.10 21:00 UTC.
  const std::int64_t D28 = utcMs(2026, 10, 27, 21);
  const std::int64_t D29 = utcMs(2026, 10, 28, 21);
  const std::int64_t D30 = utcMs(2026, 10, 29, 21);
  const auto Days =
      Source->load({kFixture, "DAYT", 86400, D28, utcMs(2026, 10, 30, 21)});
  ASSERT_EQ(Days.size(), 3u);

  // 28.10: свечи 09:59, 18:30 (разрыв между ними) и вечерняя 23:59 МСК.
  EXPECT_EQ(Days[0].Index, 0);
  EXPECT_EQ(Days[0].TsOpenMs, D28);
  EXPECT_EQ(Days[0].Open, Decimal::parse("100"));
  EXPECT_EQ(Days[0].High, Decimal::parse("103"));
  EXPECT_EQ(Days[0].Low, Decimal::parse("99"));
  EXPECT_EQ(Days[0].Close, Decimal::parse("102"));
  EXPECT_EQ(Days[0].Volume, 60);
  EXPECT_EQ(Days[0].Value, Decimal::parse("6000"));
  EXPECT_EQ(Days[0].AvailableAtMs, D29); // конец суток МСК

  // 29.10: свеча 00:00 МСК (полночь) относится к новым суткам, не к 28.10.
  EXPECT_EQ(Days[1].TsOpenMs, D29);
  EXPECT_EQ(Days[1].Open, Decimal::parse("102"));
  EXPECT_EQ(Days[1].High, Decimal::parse("105"));
  EXPECT_EQ(Days[1].Low, Decimal::parse("101.9"));
  EXPECT_EQ(Days[1].Close, Decimal::parse("104"));
  EXPECT_EQ(Days[1].Volume, 90);
  EXPECT_EQ(Days[1].Value, Decimal::parse("9000"));
  EXPECT_EQ(Days[1].AvailableAtMs, D30);

  // 30.10: единственная свеча в 00:00 МСК.
  EXPECT_EQ(Days[2].TsOpenMs, D30);
  EXPECT_EQ(Days[2].Open, Decimal::parse("110"));
  EXPECT_EQ(Days[2].Volume, 1);
  EXPECT_EQ(Days[2].AvailableAtMs, utcMs(2026, 10, 30, 21));
}

TEST_F(ClickHouseIntegration, DailyAggregatedBarNotAvailableBeforeDayEnd) {
  // Последняя свеча 29.10 - 10:00 МСК, но бар дня доступен только в 00:00 МСК
  // 30.10, а не по последней минутке (INV-1).
  const std::int64_t D28 = utcMs(2026, 10, 27, 21);
  const auto Before =
      Source->load({kFixture, "DAYT", 86400, D28, utcMs(2026, 10, 29, 20, 59)});
  ASSERT_EQ(Before.size(), 1u); // только 28.10
  EXPECT_EQ(Before[0].TsOpenMs, D28);
  // Диапазон, начинающийся в середине истории: нумерация с нуля.
  const auto Later =
      Source->load({kFixture, "DAYT", 86400, utcMs(2026, 10, 28, 21),
                    utcMs(2026, 10, 30, 21)});
  ASSERT_EQ(Later.size(), 2u);
  EXPECT_EQ(Later[0].Index, 0);
}

TEST_F(ClickHouseIntegration, CalendarFromRealDatabase) {
  const auto Days = Calendar->tradingDays(kFixture, 20261019, 20261030);
  ASSERT_EQ(Days.size(), 12u);
  EXPECT_EQ(Days.front(), (core::TradingDay{20261019, true}));
  EXPECT_EQ(Days[5], (core::TradingDay{20261024, false})); // суббота
  EXPECT_EQ(Days[6], (core::TradingDay{20261025, false}));
  EXPECT_EQ(Days.back(), (core::TradingDay{20261030, true}));

  const auto Part = Calendar->tradingDays(kFixture, 20261023, 20261026);
  ASSERT_EQ(Part.size(), 4u); // границы включаются
  EXPECT_EQ(Part.front().Yyyymmdd, 20261023);
  EXPECT_EQ(Part.back().Yyyymmdd, 20261026);

  EXPECT_TRUE(Calendar->tradingDays(kFixture, 20261030, 20261019).empty());
  EXPECT_THROW(Calendar->tradingDays("00000000-0000-4000-8000-000000000000",
                                     20261019, 20261030),
               core::SnapshotNotFound);
}

TEST_F(ClickHouseIntegration, RuoniaFromRealDatabase) {
  const auto All = Ruonia->ruoniaRates(kFixture, 20261030);
  ASSERT_EQ(All.size(), 12u);
  EXPECT_EQ(All.front().Yyyymmdd, 20261019);
  EXPECT_EQ(All.front().Rate, Decimal::parse("15"));
  EXPECT_EQ(All.back().Yyyymmdd, 20261030);
  for (std::size_t I = 1; I < All.size(); ++I)
    EXPECT_LT(All[I - 1].Yyyymmdd, All[I].Yyyymmdd);

  // Ставки позже UpToYmd не отдаются (INV-1).
  const auto Part = Ruonia->ruoniaRates(kFixture, 20261026);
  ASSERT_EQ(Part.size(), 8u);
  EXPECT_EQ(Part.back().Yyyymmdd, 20261026);
  EXPECT_TRUE(Ruonia->ruoniaRates(kFixture, 20200101).empty());

  EXPECT_THROW(
      Ruonia->ruoniaRates("00000000-0000-4000-8000-000000000000", 20261030),
      core::SnapshotNotFound);
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

TEST_F(ClickHouseIntegration, BatchLoadMatchesSingleLoadBitForBit) {
  const auto Config =
      worker::ClickHouseConfig::fromUrl(std::getenv("CLICKHOUSE_URL"));
  // Отдельный источник без кэша для эталонной одиночной загрузки (INV-4).
  worker::ClickHouseBarSource Single(Config, 0, 0);
  worker::ClickHouseBarSource Batch(Config);

  // 1. Хранимые минутные свечи (SBER, GAZP).
  const std::int64_t MFrom = utcMs(2026, 10, 28, 3, 59);
  const std::int64_t MTo = utcMs(2026, 10, 28, 8, 0);
  const auto MinuteBatch =
      Batch.loadBatch({kFixture, {"SBER", "GAZP"}, 60, MFrom, MTo});
  ASSERT_EQ(MinuteBatch.size(), 2u);
  EXPECT_EQ(MinuteBatch[0], Single.load({kFixture, "SBER", 60, MFrom, MTo}));
  EXPECT_EQ(MinuteBatch[1], Single.load({kFixture, "GAZP", 60, MFrom, MTo}));
  EXPECT_FALSE(MinuteBatch[0].empty());
  EXPECT_FALSE(MinuteBatch[1].empty());

  // 2. Агрегированные 5-минутные свечи (SBER, GAZP — обе из минутных).
  const auto FiveMinBatch =
      Batch.loadBatch({kFixture, {"SBER", "GAZP"}, 300, MFrom, MTo});
  ASSERT_EQ(FiveMinBatch.size(), 2u);
  EXPECT_EQ(FiveMinBatch[0], Single.load({kFixture, "SBER", 300, MFrom, MTo}));
  EXPECT_EQ(FiveMinBatch[1], Single.load({kFixture, "GAZP", 300, MFrom, MTo}));

  // 3. Смешанный пакет: SBER (хранимые 1d) + DAYT (агрегированные 1d из 1m).
  const std::int64_t DFrom = utcMs(2026, 10, 18, 21);
  const std::int64_t DTo = utcMs(2026, 10, 30, 21);
  const auto DailyBatch =
      Batch.loadBatch({kFixture, {"SBER", "DAYT"}, 86400, DFrom, DTo});
  ASSERT_EQ(DailyBatch.size(), 2u);
  EXPECT_EQ(DailyBatch[0], Single.load({kFixture, "SBER", 86400, DFrom, DTo}));
  EXPECT_EQ(DailyBatch[1], Single.load({kFixture, "DAYT", 86400, DFrom, DTo}));
  EXPECT_EQ(DailyBatch[0].size(), 10u);
  EXPECT_EQ(DailyBatch[1].size(), 3u);
}

} // namespace
