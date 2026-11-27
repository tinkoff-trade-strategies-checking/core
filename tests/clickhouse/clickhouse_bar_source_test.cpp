// Юнит-тесты адаптера ClickHouse без сервера: клиент подменён, проверяются SQL,
// параметры, кэш метаданных/баров, пакетная загрузка и потокобезопасность.
#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "clickhouse/clickhouse_bar_source.hpp"

namespace {

using core::Decimal;
using worker::QueryParams;

constexpr const char *kSnap = "ec6b3c97-7036-56cf-ba4d-ec4e919e682d";

struct Call {
  std::string Sql;
  QueryParams Params;
};

// Потокобезопасный подменный клиент: отвечает по подстроке SQL, запоминает все
// вызовы.
class FakeClient final : public worker::ClickHouseClient {
public:
  using HandlerFn =
      std::function<std::string(const std::string &Sql, const QueryParams &)>;
  FakeClient(std::shared_ptr<std::vector<Call>> CallLog,
             std::shared_ptr<std::mutex> LogMutex, HandlerFn Handler)
      : Calls(std::move(CallLog)), Mutex(std::move(LogMutex)),
        Respond(std::move(Handler)) {}
  std::string query(const std::string &Sql,
                    const QueryParams &Params) override {
    {
      std::lock_guard<std::mutex> Lock(*Mutex);
      Calls->push_back({Sql, Params});
    }
    return Respond(Sql, Params);
  }

private:
  std::shared_ptr<std::vector<Call>> Calls;
  std::shared_ptr<std::mutex> Mutex;
  HandlerFn Respond;
};

struct Fixture {
  std::shared_ptr<std::vector<Call>> Calls =
      std::make_shared<std::vector<Call>>();
  std::shared_ptr<std::mutex> Mutex = std::make_shared<std::mutex>();
  std::string ReadySnapshots = "1\n";
  std::string SecuritiesRows = "SBER\tФинансы\t1\t0.010000\n"
                               "GAZP\tЭнергетика\t10\t0.010000\n"
                               "LKOH\tНефть и газ\t1\t0.500000\n"
                               "SYNTH\tТест\t1\t0.010000\n";
  std::string StoredIntervalsRows = "SBER\t60\nSBER\t86400\n"
                                    "GAZP\t60\nGAZP\t86400\n"
                                    "LKOH\t60\nLKOH\t86400\n"
                                    "SYNTH\t60\n";
  std::string Rows;
  FakeClient::HandlerFn CustomDataHandler;

  worker::ClickHouseBarSource
  make(std::size_t MaxCacheEntries =
           worker::ClickHouseBarSource::kDefaultBarCacheEntries,
       std::size_t MaxCacheBars =
           worker::ClickHouseBarSource::kDefaultBarCacheMaxBars) {
    return worker::ClickHouseBarSource(
        std::make_unique<FakeClient>(
            Calls, Mutex,
            [this](const std::string &Sql,
                   const QueryParams &Params) -> std::string {
              if (Sql.find("FROM snapshots_ready") != std::string::npos)
                return ReadySnapshots;
              if (Sql.find("FROM securities") != std::string::npos)
                return SecuritiesRows;
              if (Sql.find(
                      "SELECT DISTINCT secid, interval_sec FROM candles") !=
                  std::string::npos)
                return StoredIntervalsRows;
              if (CustomDataHandler)
                return CustomDataHandler(Sql, Params);
              return Rows;
            }),
        MaxCacheEntries, MaxCacheBars);
  }

  bool usedSql(const std::string &Part) const {
    std::lock_guard<std::mutex> Lock(*Mutex);
    for (const auto &C : *Calls) {
      if (C.Sql.find(Part) != std::string::npos)
        return true;
    }
    return false;
  }

  std::size_t countSql(const std::string &Part) const {
    std::lock_guard<std::mutex> Lock(*Mutex);
    std::size_t Count = 0;
    for (const auto &C : *Calls) {
      if (C.Sql.find(Part) != std::string::npos)
        ++Count;
    }
    return Count;
  }
};

TEST(ClickHouseUrl, ParsesCredentialsAndDatabase) {
  const auto Cfg = worker::ClickHouseConfig::fromUrl(
      "http://ttsc:p%40ss@127.0.0.1:8123/ttsc");
  EXPECT_EQ(Cfg.BaseUrl, "http://127.0.0.1:8123");
  EXPECT_EQ(Cfg.User, "ttsc");
  EXPECT_EQ(Cfg.Password, "p@ss");
  EXPECT_EQ(Cfg.Database, "ttsc");

  const auto Bare = worker::ClickHouseConfig::fromUrl("http://clickhouse:8123");
  EXPECT_EQ(Bare.BaseUrl, "http://clickhouse:8123");
  EXPECT_EQ(Bare.Database, "default");
  EXPECT_TRUE(Bare.User.empty());

  EXPECT_THROW(worker::ClickHouseConfig::fromUrl("127.0.0.1:8123"),
               std::invalid_argument);
  EXPECT_THROW(worker::ClickHouseConfig::fromUrl("ftp://host"),
               std::invalid_argument);
}

// Пароль из CLICKHOUSE_URL не попадает в текст ошибки разбора (воркер печатает
// его в журнал контейнера).
TEST(ClickHouseUrl, ParseErrorsDoNotEchoCredentials) {
  for (const char *Url : {"ttsc:hunter2@127.0.0.1:8123", "http://ttsc:hunter2@",
                          "ftp://ttsc:hunter2@host"}) {
    try {
      worker::ClickHouseConfig::fromUrl(Url);
      FAIL() << Url;
    } catch (const std::invalid_argument &E) {
      EXPECT_EQ(std::string(E.what()).find("hunter2"), std::string::npos)
          << E.what();
    }
  }
}

TEST(TabSeparated, SplitsAndUnescapes) {
  const auto Rows = worker::parseTabSeparated("a\tb\\tc\\\\d\n\t\\n\n");
  ASSERT_EQ(Rows.size(), 2u);
  ASSERT_EQ(Rows[0].size(), 2u);
  EXPECT_EQ(Rows[0][0], "a");
  EXPECT_EQ(Rows[0][1], "b\tc\\d");
  ASSERT_EQ(Rows[1].size(), 2u);
  EXPECT_EQ(Rows[1][0], "");
  EXPECT_EQ(Rows[1][1], "\n");
  EXPECT_TRUE(worker::parseTabSeparated("").empty());
}

TEST(ClickHouseBarSource, LoadsStoredCandlesWithExactDecimals) {
  Fixture F;
  F.Rows = "1792000000000\t1792000060000\t298.210000\t298.940000\t298."
           "210000\t298.580000\t29810\t8903444.160000\n"
           "1792000060000\t1792000120000\t298.550000\t298.580000\t298."
           "440000\t298.480000\t15152\t4522838.080000\n";
  auto Source = F.make();
  const auto Bars =
      Source.load({kSnap, "SBER", 60, 1792000000000, 1792000999000});
  ASSERT_EQ(Bars.size(), 2u);
  EXPECT_EQ(Bars[0].Index, 0);
  EXPECT_EQ(Bars[1].Index, 1);
  EXPECT_EQ(Bars[0].TsOpenMs, 1792000000000);
  EXPECT_EQ(Bars[0].AvailableAtMs, 1792000060000);
  EXPECT_EQ(Bars[0].Open, Decimal::parse("298.21"));
  EXPECT_EQ(Bars[0].High, Decimal::parse("298.94"));
  EXPECT_EQ(Bars[0].Close, Decimal::parse("298.58"));
  EXPECT_EQ(Bars[0].Volume, 29810);
  EXPECT_EQ(Bars[0].Value, Decimal::parse("8903444.16"));
  EXPECT_TRUE(F.usedSql("FROM candles WHERE"));
  EXPECT_FALSE(F.usedSql("candles_from_trades"));
}

TEST(ClickHouseBarSource, FallsBackToMinuteAggregationWhenIntervalNotStored) {
  Fixture F;
  F.StoredIntervalsRows = "SYNTH\t60\n";
  F.Rows = "1792000000000\t1792000060000\t100.000000\t101.500000\t99."
           "500000\t99.500000\t42\t4224.000000\n";
  auto Source = F.make();
  const auto Bars = Source.load({kSnap, "SYNTH", 300, 0, 2000000000000});
  ASSERT_EQ(Bars.size(), 1u);
  EXPECT_EQ(Bars[0].Volume, 42);
  EXPECT_TRUE(F.usedSql("interval_sec = 60"));
  EXPECT_TRUE(F.usedSql("intDiv(toUnixTimestamp64Milli(ts_open)"));
  EXPECT_FALSE(F.usedSql("candles_from_trades"));

  // 1 д — группировка по дню МСК
  Fixture FDay;
  FDay.StoredIntervalsRows = "SYNTH\t60\n";
  FDay.Rows = "1792000000000\t1792000060000\t100.000000\t101.500000\t99."
              "500000\t99.500000\t42\t4224.000000\n";
  auto SourceDay = FDay.make();
  SourceDay.load({kSnap, "SYNTH", 86400, 0, 2000000000000});
  EXPECT_TRUE(FDay.usedSql("toStartOfDay(ts_open, 'Europe/Moscow')"));
  EXPECT_FALSE(FDay.usedSql("candles_from_trades"));
}

TEST(ClickHouseBarSource, NoSqlInjectionThroughSecid) {
  Fixture F;
  const std::string Evil = "X' OR 1=1; DROP TABLE candles --";
  F.SecuritiesRows += Evil + "\tТест\t1\t0.01\n";
  F.StoredIntervalsRows += Evil + "\t60\n";
  auto Source = F.make();
  Source.load({kSnap, Evil, 60, 0, 1000});
  for (const auto &Call : *F.Calls) {
    EXPECT_EQ(Call.Sql.find("DROP"), std::string::npos) << Call.Sql;
    EXPECT_EQ(Call.Sql.find("OR 1=1"), std::string::npos) << Call.Sql;
  }
  bool PassedAsParam = false;
  for (const auto &Call : *F.Calls) {
    for (const auto &[Name, Value] : Call.Params) {
      if (Name == "secid" && Value == Evil)
        PassedAsParam = true;
    }
  }
  EXPECT_TRUE(PassedAsParam);

  // Пакетный запрос также передаёт secid только через параметр Array(String).
  Source.loadBatch({kSnap, {"SBER", Evil}, 60, 0, 2000});
  for (const auto &Call : *F.Calls) {
    EXPECT_EQ(Call.Sql.find("DROP"), std::string::npos) << Call.Sql;
    EXPECT_EQ(Call.Sql.find("OR 1=1"), std::string::npos) << Call.Sql;
  }
  bool PassedInArrayParam = false;
  for (const auto &[Name, Value] : F.Calls->back().Params) {
    if (Name == "secids" && Value.find("\\'") != std::string::npos)
      PassedInArrayParam = true;
  }
  EXPECT_TRUE(PassedInArrayParam);
}

TEST(ClickHouseBarSource, RangeAndIntervalGoAsParameters) {
  Fixture F;
  auto Source = F.make();
  Source.load({kSnap, "SBER", 60, 123, 456});
  const Call &Last = F.Calls->back();
  bool From = false, To = false, Interval = false;
  for (const auto &[Name, Value] : Last.Params) {
    if (Name == "from_ms" && Value == "123")
      From = true;
    if (Name == "to_ms" && Value == "456")
      To = true;
    if (Name == "interval_sec" && Value == "60")
      Interval = true;
  }
  EXPECT_TRUE(From && To && Interval);
  EXPECT_NE(Last.Sql.find("ORDER BY ts_open ASC"),
            std::string::npos); // INV-4: порядок задан явно
}

TEST(ClickHouseBarSource, OnlyReadySnapshots) {
  Fixture F;
  F.ReadySnapshots = "0\n";
  auto Source = F.make();
  EXPECT_THROW(Source.load({kSnap, "SBER", 60, 0, 1000}),
               core::SnapshotNotFound);
  EXPECT_THROW(Source.security(kSnap, "SBER"), core::SnapshotNotFound);
  EXPECT_TRUE(F.usedSql("snapshots_ready"));
}

TEST(ClickHouseBarSource, MalformedSnapshotIdNeverReachesServer) {
  Fixture F;
  auto Source = F.make();
  EXPECT_THROW(Source.load({"not-a-uuid", "SBER", 60, 0, 1000}),
               core::SnapshotNotFound);
  EXPECT_TRUE(F.Calls->empty());
}

TEST(ClickHouseBarSource, UnknownSecurity) {
  Fixture F;
  auto Source = F.make();
  EXPECT_THROW(Source.load({kSnap, "NOPE", 60, 0, 1000}),
               core::SecurityNotFound);
}

TEST(ClickHouseBarSource, EmptyRangeIsEmptyNotError) {
  Fixture F;
  auto Source = F.make();
  EXPECT_TRUE(Source.load({kSnap, "SBER", 60, 1000, 1000}).empty());
  F.Rows = "";
  EXPECT_TRUE(Source.load({kSnap, "SBER", 60, 0, 1000}).empty());
}

TEST(ClickHouseBarSource, InvalidIntervalRejections) {
  Fixture F;
  F.StoredIntervalsRows = "";
  auto Source = F.make();
  EXPECT_THROW(Source.load({kSnap, "SBER", 604800, 0, 1000}),
               worker::IntervalUnavailable);
  EXPECT_THROW(Source.load({kSnap, "SBER", 10, 0, 1000}),
               worker::IntervalUnavailable);
  EXPECT_THROW(Source.load({kSnap, "SBER", 75, 0, 1000}),
               worker::IntervalUnavailable);
  EXPECT_THROW(Source.load({kSnap, "SBER", 0, 0, 1000}), std::invalid_argument);
}

TEST(ClickHouseBarSource, RejectsBadServerData) {
  Fixture F;
  auto Source = F.make();
  F.Rows = "1\t2\t1.0\t1.0\t1.0\n"; // не хватает столбцов
  EXPECT_THROW(Source.load({kSnap, "SBER", 60, 0, 1000}),
               core::MarketDataError);
  F.Rows = "1\t2\tabc\t1\t1\t1\t1\t1\n"; // не число
  EXPECT_THROW(Source.load({kSnap, "SBER", 60, 0, 1000}),
               core::MarketDataError);
  F.Rows =
      "5\t6\t1\t1\t1\t1\t1\t1\n5\t6\t1\t1\t1\t1\t1\t1\n"; // дубликат ts_open
  EXPECT_THROW(Source.load({kSnap, "SBER", 60, 0, 1000}),
               core::MarketDataError);
}

TEST(ClickHouseBarSource, AmbiguousSecurityIsAnError) {
  // Один secid на двух площадках в версии данных: выбор «первой» записи
  // недетерминирован (INV-4).
  Fixture F;
  F.SecuritiesRows = "SBER\tФинансы\t1\t0.01\nSBER\tФинансы\t1\t0.01\n";
  auto Source = F.make();
  EXPECT_THROW(Source.security(kSnap, "SBER"), core::MarketDataError);
}

TEST(ClickHouseBarSource, AmbiguousSecurityInLoadIsAnError) {
  // Неоднозначный инструмент в load() должен отклоняться так же, как и в
  // security() (INV-4).
  Fixture F;
  F.SecuritiesRows = "SBER\tФинансы\t1\t0.01\nSBER\tФинансы\t1\t0.01\n";
  auto Source = F.make();
  EXPECT_THROW(Source.load({kSnap, "SBER", 60, 0, 1000}),
               core::MarketDataError);
}

TEST(ClickHouseBarSource, TruncatedResponseIsAnError) {
  Fixture F;
  auto Source = F.make();
  F.Rows =
      "1792000000000\t1792000060000\t1.0\t1.0\t1.0\t1.0\t1\t1.0"; // нет
                                                                  // завершающего
                                                                  // перевода
                                                                  // строки
  EXPECT_THROW(Source.load({kSnap, "SBER", 60, 0, 1792000999000}),
               worker::ClickHouseError);
  EXPECT_THROW(worker::parseTabSeparated("a\tb"), worker::ClickHouseError);
  EXPECT_NO_THROW(worker::parseTabSeparated("a\tb\n"));
}

TEST(ClickHouseBarSource, OnlyBarsAvailableByToMs) {
  Fixture F;
  auto Source = F.make();
  Source.load({kSnap, "SBER", 60, 0, 456});
  EXPECT_NE(F.Calls->back().Sql.find(
                "available_at <= fromUnixTimestamp64Milli({to_ms:Int64}"),
            std::string::npos);
  // Защита на стороне клиента: сервер вернул бар, который к toMs ещё не
  // известен, либо available_at < ts_open.
  F.Rows = "100\t999\t1\t1\t1\t1\t1\t1\n";
  EXPECT_THROW(Source.load({kSnap, "SBER", 60, 1, 456}), core::MarketDataError);
  F.Rows = "100\t50\t1\t1\t1\t1\t1\t1\n";
  EXPECT_THROW(Source.load({kSnap, "SBER", 60, 2, 456}), core::MarketDataError);
}

TEST(ClickHouseBarSource, ParsesSecurity) {
  Fixture F;
  auto Source = F.make();
  const auto Sec = Source.security(kSnap, "LKOH");
  EXPECT_EQ(Sec.Secid, "LKOH");
  EXPECT_EQ(Sec.Sector, "Нефть и газ");
  EXPECT_EQ(Sec.LotSize, 1);
  EXPECT_EQ(Sec.PriceStep, Decimal::parse("0.5"));
}

TEST(ClickHouseBarSource, AggregationParsesOhlcv) {
  Fixture F;
  F.StoredIntervalsRows = "SBER\t60\n";
  F.Rows = "1792000000000\t1792000300000\t100.000000\t105.500000\t99."
           "500000\t103.000000\t150\t15300.000000\n";
  auto Source = F.make();
  const auto Bars =
      Source.load({kSnap, "SBER", 300, 1792000000000, 1792000600000});
  ASSERT_EQ(Bars.size(), 1u);
  EXPECT_EQ(Bars[0].Index, 0);
  EXPECT_EQ(Bars[0].TsOpenMs, 1792000000000);
  EXPECT_EQ(Bars[0].AvailableAtMs, 1792000300000);
  EXPECT_EQ(Bars[0].Open, Decimal::parse("100.0"));
  EXPECT_EQ(Bars[0].High, Decimal::parse("105.5"));
  EXPECT_EQ(Bars[0].Low, Decimal::parse("99.5"));
  EXPECT_EQ(Bars[0].Close, Decimal::parse("103.0"));
  EXPECT_EQ(Bars[0].Volume, 150);
  EXPECT_EQ(Bars[0].Value, Decimal::parse("15300.0"));
}

TEST(ClickHouseBarSource, AggregationParametersPassedCorrectly) {
  Fixture F;
  F.StoredIntervalsRows = "GAZP\t60\n";
  auto Source = F.make();
  Source.load({kSnap, "GAZP", 900, 1000000, 2000000});
  const Call &Last = F.Calls->back();
  bool HasIntervalMs = false;
  bool HasFromMs = false;
  bool HasToMs = false;
  for (const auto &[Name, Value] : Last.Params) {
    if (Name == "interval_ms" && Value == "900000")
      HasIntervalMs = true;
    if (Name == "from_ms" && Value == "1000000")
      HasFromMs = true;
    if (Name == "to_ms" && Value == "2000000")
      HasToMs = true;
  }
  EXPECT_TRUE(HasIntervalMs && HasFromMs && HasToMs);
  EXPECT_NE(Last.Sql.find("ORDER BY bar_ts_open ASC"), std::string::npos);
  // Бар [T, T+Δ) доступен в T+Δ, а не по последней минутке (INV-1).
  EXPECT_NE(Last.Sql.find("bar_ts_open + {interval_ms:Int64} AS "
                          "bar_available_at"),
            std::string::npos);
  EXPECT_EQ(Last.Sql.find("argMax(available_at"), std::string::npos);
  // Верхняя граница в WHERE (core#27), чтобы не сканировать весь снапшот
  EXPECT_NE(Last.Sql.find("ts_open < fromUnixTimestamp64Milli({to_ms:Int64} + "
                          "{interval_ms:Int64}, 'UTC')"),
            std::string::npos);
}

TEST(ClickHouseBarSource, DailyAggregationParametersAndSql) {
  Fixture F;
  F.StoredIntervalsRows = "GAZP\t60\n";
  auto Source = F.make();
  Source.load({kSnap, "GAZP", 86400, 1000000, 2000000});
  const Call &Last = F.Calls->back();
  bool HasIntervalMs = false;
  bool HasFromMs = false;
  bool HasToMs = false;
  for (const auto &[Name, Value] : Last.Params) {
    if (Name == "interval_ms" && Value == "86400000")
      HasIntervalMs = true;
    if (Name == "from_ms" && Value == "1000000")
      HasFromMs = true;
    if (Name == "to_ms" && Value == "2000000")
      HasToMs = true;
  }
  EXPECT_TRUE(HasIntervalMs && HasFromMs && HasToMs);
  EXPECT_NE(Last.Sql.find("toStartOfDay"), std::string::npos);
  // Дневной бар доступен в конце суток МСК (T + 86400 с).
  EXPECT_NE(Last.Sql.find("bar_ts_open + {interval_ms:Int64} AS "
                          "bar_available_at"),
            std::string::npos);
  EXPECT_EQ(Last.Sql.find("argMax(available_at"), std::string::npos);
  // Верхняя граница в WHERE (core#27)
  EXPECT_NE(Last.Sql.find("ts_open < fromUnixTimestamp64Milli({to_ms:Int64} + "
                          "{interval_ms:Int64}, 'UTC')"),
            std::string::npos);
}

TEST(ClickHouseBarSource, CachesImmutableSnapshotMetadata) {
  Fixture F;
  F.Rows = "100\t160\t10.0\t11.0\t9.0\t10.5\t100\t1050.0\n";
  auto Source = F.make();

  // Многократные вызовы security() и load() для одного снапшота (INV-5).
  EXPECT_EQ(Source.security(kSnap, "SBER").LotSize, 1);
  EXPECT_EQ(Source.security(kSnap, "GAZP").LotSize, 10);
  EXPECT_EQ(Source.security(kSnap, "LKOH").PriceStep, Decimal::parse("0.5"));
  EXPECT_EQ(Source.load({kSnap, "SBER", 60, 0, 1000}).size(), 1u);
  EXPECT_EQ(Source.load({kSnap, "GAZP", 60, 0, 1000}).size(), 1u);
  EXPECT_EQ(Source.load({kSnap, "LKOH", 60, 0, 2000}).size(), 1u);

  EXPECT_EQ(F.countSql("FROM snapshots_ready"), 1u);
  EXPECT_EQ(F.countSql("FROM securities"), 1u);
  EXPECT_EQ(F.countSql("SELECT DISTINCT secid, interval_sec FROM candles"), 1u);
}

TEST(ClickHouseBarSource, BarCacheHitsAndLruEviction) {
  Fixture F;
  F.Rows = "100\t160\t10.0\t11.0\t9.0\t10.5\t100\t1050.0\n";
  // Ёмкость 2 записи: проверяем попадание и вытеснение LRU.
  auto Source = F.make(/*MaxCacheEntries=*/2, /*MaxCacheBars=*/100);

  const auto A1 = Source.load({kSnap, "SBER", 60, 0, 1000});
  const auto B1 = Source.load({kSnap, "GAZP", 60, 0, 1000});
  EXPECT_EQ(F.countSql("ORDER BY ts_open ASC"), 2u);

  // Попадание по A перемещает A в MRU (B становится LRU).
  const auto A2 = Source.load({kSnap, "SBER", 60, 0, 1000});
  EXPECT_EQ(A1, A2);
  EXPECT_EQ(F.countSql("ORDER BY ts_open ASC"), 2u);

  // Загрузка C вытесняет B, но сохраняет A.
  (void)Source.load({kSnap, "LKOH", 60, 0, 1000});
  EXPECT_EQ(F.countSql("ORDER BY ts_open ASC"), 3u);

  // A всё ещё в кэше.
  EXPECT_EQ(Source.load({kSnap, "SBER", 60, 0, 1000}), A1);
  EXPECT_EQ(F.countSql("ORDER BY ts_open ASC"), 3u);

  // B был вытеснен — повторный запрос к ClickHouse.
  EXPECT_EQ(Source.load({kSnap, "GAZP", 60, 0, 1000}), B1);
  EXPECT_EQ(F.countSql("ORDER BY ts_open ASC"), 4u);

  // Ограничение по числу баров: серия длиннее MaxCacheBars не кэшируется.
  Fixture FMax;
  FMax.Rows = "100\t160\t10\t11\t9\t10\t10\t100\n"
              "200\t260\t10\t11\t9\t10\t10\t100\n"
              "300\t360\t10\t11\t9\t10\t10\t100\n";
  auto Small = FMax.make(/*MaxCacheEntries=*/16, /*MaxCacheBars=*/2);
  EXPECT_EQ(Small.load({kSnap, "SBER", 60, 0, 1000}).size(), 3u);
  EXPECT_EQ(Small.load({kSnap, "SBER", 60, 0, 1000}).size(), 3u);
  EXPECT_EQ(FMax.countSql("ORDER BY ts_open ASC"), 2u);
}

TEST(ClickHouseBarSource, LoadBatchSingleQueryAndParityWithSingleLoad) {
  const std::string SberRow =
      "1000\t1060\t298.210000\t298.940000\t298.210000\t298.580000\t100\t29858."
      "000000\n";
  const std::string GazpRow =
      "1000\t1060\t150.000000\t151.000000\t149.500000\t150.500000\t200\t30100."
      "000000\n"
      "2000\t2060\t150.500000\t152.000000\t150.000000\t151.800000\t300\t45540."
      "000000\n";

  auto MakeDataHandler = [&](const std::string &Sql,
                             const QueryParams &Params) -> std::string {
    if (Sql.find("secid IN {secids:Array(String)}") != std::string::npos) {
      std::string Out;
      for (const auto &[Name, Value] : Params) {
        if (Name == "secids") {
          if (Value.find("'GAZP'") != std::string::npos) {
            Out += "GAZP\t1000\t1060\t150.000000\t151.000000\t149.500000\t"
                   "150.500000\t200\t30100.000000\n"
                   "GAZP\t2000\t2060\t150.500000\t152.000000\t150.000000\t"
                   "151.800000\t300\t45540.000000\n";
          }
          if (Value.find("'SBER'") != std::string::npos) {
            Out += "SBER\t" + SberRow;
          }
        }
      }
      return Out;
    }
    for (const auto &[Name, Value] : Params) {
      if (Name == "secid") {
        if (Value == "SBER")
          return SberRow;
        if (Value == "GAZP")
          return GazpRow;
      }
    }
    return "";
  };

  Fixture FSingle;
  FSingle.CustomDataHandler = MakeDataHandler;
  auto SingleSource = FSingle.make(0, 0);

  Fixture FBatch;
  FBatch.CustomDataHandler = MakeDataHandler;
  auto BatchSource = FBatch.make();

  // Хранимые свечи: 3 бумаги (у LKOH нет баров в окне) — ровно 1 пакетный
  // запрос.
  const auto BatchBars =
      BatchSource.loadBatch({kSnap, {"SBER", "GAZP", "LKOH"}, 60, 0, 5000});
  ASSERT_EQ(BatchBars.size(), 3u);
  EXPECT_EQ(BatchBars[0], SingleSource.load({kSnap, "SBER", 60, 0, 5000}));
  EXPECT_EQ(BatchBars[1], SingleSource.load({kSnap, "GAZP", 60, 0, 5000}));
  EXPECT_EQ(BatchBars[2], SingleSource.load({kSnap, "LKOH", 60, 0, 5000}));
  EXPECT_EQ(FBatch.countSql("secid IN {secids:Array(String)}"), 1u);

  // Повторный вызов loadBatch полностью обслуживается из LRU-кэша (0 новых
  // запросов).
  const std::size_t CallsBefore = FBatch.Calls->size();
  const auto CachedBatch =
      BatchSource.loadBatch({kSnap, {"LKOH", "SBER", "GAZP"}, 60, 0, 5000});
  EXPECT_EQ(FBatch.Calls->size(), CallsBefore);
  EXPECT_EQ(CachedBatch[0], BatchBars[2]);
  EXPECT_EQ(CachedBatch[1], BatchBars[0]);
  EXPECT_EQ(CachedBatch[2], BatchBars[1]);

  // Пакетная агрегация (300 с — нет в StoredIntervalsRows): GROUP BY secid,
  // bar_ts_open.
  const auto AggBatch =
      BatchSource.loadBatch({kSnap, {"SBER", "GAZP"}, 300, 0, 5000});
  ASSERT_EQ(AggBatch.size(), 2u);
  EXPECT_EQ(AggBatch[0], SingleSource.load({kSnap, "SBER", 300, 0, 5000}));
  EXPECT_EQ(AggBatch[1], SingleSource.load({kSnap, "GAZP", 300, 0, 5000}));
  EXPECT_EQ(FBatch.countSql("GROUP BY secid, bar_ts_open"), 1u);
}

TEST(ClickHouseBarSource, ThreadSafeConcurrentAccess) {
  Fixture F;
  F.CustomDataHandler = [](const std::string &Sql,
                           const QueryParams &Params) -> std::string {
    if (Sql.find("secid IN {secids:Array(String)}") != std::string::npos) {
      return "GAZP\t1000\t1060\t150\t151\t149\t150\t20\t3000\n"
             "SBER\t1000\t1060\t298\t299\t297\t298\t10\t2980\n";
    }
    for (const auto &[Name, Value] : Params) {
      if (Name == "secid" && Value == "GAZP")
        return "1000\t1060\t150\t151\t149\t150\t20\t3000\n";
      if (Name == "secid" && Value == "SBER")
        return "1000\t1060\t298\t299\t297\t298\t10\t2980\n";
    }
    return "";
  };
  auto Source = F.make();

  std::atomic<int> Failures{0};
  std::vector<std::thread> Threads;
  for (int T = 0; T < 8; ++T) {
    Threads.emplace_back([&, T]() {
      try {
        for (int I = 0; I < 20; ++I) {
          const auto Sec = Source.security(kSnap, T % 2 == 0 ? "SBER" : "GAZP");
          if (Sec.Secid.empty())
            ++Failures;
          const auto Single = Source.load({kSnap, "SBER", 60, 0, 2000});
          const auto Batch =
              Source.loadBatch({kSnap, {"SBER", "GAZP"}, 60, 0, 2000});
          if (Single.size() != 1u || Batch.size() != 2u || Batch[0] != Single ||
              Batch[1].size() != 1u) {
            ++Failures;
          }
        }
      } catch (...) {
        ++Failures;
      }
    });
  }
  for (auto &Th : Threads)
    Th.join();

  EXPECT_EQ(Failures.load(), 0);
}

} // namespace
