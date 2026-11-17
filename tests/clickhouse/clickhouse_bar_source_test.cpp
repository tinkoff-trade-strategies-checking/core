// Юнит-тесты адаптера ClickHouse без сервера: клиент подменён, проверяются SQL,
// параметры и разбор.
#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "clickhouse/clickhouse_bar_source.hpp"

namespace {

using core::Decimal;
using worker::QueryParams;

constexpr const char *kSnap = "ec6b3c97-7036-56cf-ba4d-ec4e919e682d";

struct Call {
  std::string sql;
  QueryParams params;
};

// Подменный клиент: отвечает по подстроке SQL, запоминает все вызовы.
class FakeClient final : public worker::ClickHouseClient {
public:
  using Handler =
      std::function<std::string(const std::string &sql, const QueryParams &)>;
  FakeClient(std::shared_ptr<std::vector<Call>> calls, Handler handler)
      : calls_(std::move(calls)), handler_(std::move(handler)) {}
  std::string query(const std::string &sql,
                    const QueryParams &params) override {
    calls_->push_back({sql, params});
    return handler_(sql, params);
  }

private:
  std::shared_ptr<std::vector<Call>> calls_;
  Handler handler_;
};

struct Fixture {
  std::shared_ptr<std::vector<Call>> calls =
      std::make_shared<std::vector<Call>>();
  std::string readySnapshots = "1\n";
  std::string securityCount = "1\n";
  std::string candleCount = "5\n";
  std::string rows;

  worker::ClickHouseBarSource make() {
    return worker::ClickHouseBarSource(std::make_unique<FakeClient>(
        calls,
        [this](const std::string &sql, const QueryParams &) -> std::string {
          if (sql.find("FROM snapshots_ready") != std::string::npos)
            return readySnapshots;
          if (sql.find("SELECT count() FROM securities") != std::string::npos)
            return securityCount;
          if (sql.find("SELECT count() FROM candles") != std::string::npos)
            return candleCount;
          return rows;
        }));
  }
  bool usedSql(const std::string &part) const {
    for (const auto &c : *calls) {
      if (c.sql.find(part) != std::string::npos)
        return true;
    }
    return false;
  }
};

TEST(ClickHouseUrl, ParsesCredentialsAndDatabase) {
  const auto cfg = worker::ClickHouseConfig::fromUrl(
      "http://ttsc:p%40ss@127.0.0.1:8123/ttsc");
  EXPECT_EQ(cfg.baseUrl, "http://127.0.0.1:8123");
  EXPECT_EQ(cfg.user, "ttsc");
  EXPECT_EQ(cfg.password, "p@ss");
  EXPECT_EQ(cfg.database, "ttsc");

  const auto bare = worker::ClickHouseConfig::fromUrl("http://clickhouse:8123");
  EXPECT_EQ(bare.baseUrl, "http://clickhouse:8123");
  EXPECT_EQ(bare.database, "default");
  EXPECT_TRUE(bare.user.empty());

  EXPECT_THROW(worker::ClickHouseConfig::fromUrl("127.0.0.1:8123"),
               std::invalid_argument);
  EXPECT_THROW(worker::ClickHouseConfig::fromUrl("ftp://host"),
               std::invalid_argument);
}

// Пароль из CLICKHOUSE_URL не попадает в текст ошибки разбора (воркер печатает
// его в журнал контейнера).
TEST(ClickHouseUrl, ParseErrorsDoNotEchoCredentials) {
  for (const char *url : {"ttsc:hunter2@127.0.0.1:8123", "http://ttsc:hunter2@",
                          "ftp://ttsc:hunter2@host"}) {
    try {
      worker::ClickHouseConfig::fromUrl(url);
      FAIL() << url;
    } catch (const std::invalid_argument &e) {
      EXPECT_EQ(std::string(e.what()).find("hunter2"), std::string::npos)
          << e.what();
    }
  }
}

TEST(TabSeparated, SplitsAndUnescapes) {
  const auto rows = worker::parseTabSeparated("a\tb\\tc\\\\d\n\t\\n\n");
  ASSERT_EQ(rows.size(), 2u);
  ASSERT_EQ(rows[0].size(), 2u);
  EXPECT_EQ(rows[0][0], "a");
  EXPECT_EQ(rows[0][1], "b\tc\\d");
  ASSERT_EQ(rows[1].size(), 2u);
  EXPECT_EQ(rows[1][0], "");
  EXPECT_EQ(rows[1][1], "\n");
  EXPECT_TRUE(worker::parseTabSeparated("").empty());
}

TEST(ClickHouseBarSource, LoadsStoredCandlesWithExactDecimals) {
  Fixture f;
  f.rows = "1792000000000\t1792000060000\t298.210000\t298.940000\t298."
           "210000\t298.580000\t29810\t8903444.160000\n"
           "1792000060000\t1792000120000\t298.550000\t298.580000\t298."
           "440000\t298.480000\t15152\t4522838.080000\n";
  auto source = f.make();
  const auto bars =
      source.load({kSnap, "SBER", 60, 1792000000000, 1792000999000});
  ASSERT_EQ(bars.size(), 2u);
  EXPECT_EQ(bars[0].index, 0);
  EXPECT_EQ(bars[1].index, 1);
  EXPECT_EQ(bars[0].tsOpenMs, 1792000000000);
  EXPECT_EQ(bars[0].availableAtMs, 1792000060000);
  EXPECT_EQ(bars[0].open, Decimal::parse("298.21"));
  EXPECT_EQ(bars[0].high, Decimal::parse("298.94"));
  EXPECT_EQ(bars[0].close, Decimal::parse("298.58"));
  EXPECT_EQ(bars[0].volume, 29810);
  EXPECT_EQ(bars[0].value, Decimal::parse("8903444.16"));
  EXPECT_TRUE(f.usedSql("FROM candles WHERE"));
  EXPECT_FALSE(f.usedSql("candles_from_trades"));
}

TEST(ClickHouseBarSource, FallsBackToTradesWhenIntervalNotStored) {
  Fixture f;
  f.candleCount = "0\n";
  f.rows = "1792000000000\t1792000060000\t100.000000\t101.500000\t99."
           "500000\t99.500000\t42\t4224.000000\n";
  auto source = f.make();
  const auto bars = source.load({kSnap, "SYNTH", 60, 0, 2000000000000});
  ASSERT_EQ(bars.size(), 1u);
  EXPECT_EQ(bars[0].volume, 42);
  EXPECT_TRUE(f.usedSql("candles_from_trades"));
}

TEST(ClickHouseBarSource, NoSqlInjectionThroughSecid) {
  Fixture f;
  auto source = f.make();
  const std::string evil = "X' OR 1=1; DROP TABLE candles --";
  source.load({kSnap, evil, 60, 0, 1000});
  for (const auto &call : *f.calls) {
    EXPECT_EQ(call.sql.find("DROP"), std::string::npos) << call.sql;
    EXPECT_EQ(call.sql.find("OR 1=1"), std::string::npos) << call.sql;
  }
  bool passedAsParam = false;
  for (const auto &call : *f.calls) {
    for (const auto &[name, value] : call.params) {
      if (name == "secid" && value == evil)
        passedAsParam = true;
    }
  }
  EXPECT_TRUE(passedAsParam);
}

TEST(ClickHouseBarSource, RangeAndIntervalGoAsParameters) {
  Fixture f;
  auto source = f.make();
  source.load({kSnap, "SBER", 60, 123, 456});
  const Call &last = f.calls->back();
  bool from = false, to = false, interval = false;
  for (const auto &[name, value] : last.params) {
    if (name == "from_ms" && value == "123")
      from = true;
    if (name == "to_ms" && value == "456")
      to = true;
    if (name == "interval_sec" && value == "60")
      interval = true;
  }
  EXPECT_TRUE(from && to && interval);
  EXPECT_NE(last.sql.find("ORDER BY ts_open ASC"),
            std::string::npos); // INV-4: порядок задан явно
}

TEST(ClickHouseBarSource, OnlyReadySnapshots) {
  Fixture f;
  f.readySnapshots = "0\n";
  auto source = f.make();
  EXPECT_THROW(source.load({kSnap, "SBER", 60, 0, 1000}),
               core::SnapshotNotFound);
  EXPECT_THROW(source.security(kSnap, "SBER"), core::SnapshotNotFound);
  EXPECT_TRUE(f.usedSql("snapshots_ready"));
}

TEST(ClickHouseBarSource, MalformedSnapshotIdNeverReachesServer) {
  Fixture f;
  auto source = f.make();
  EXPECT_THROW(source.load({"not-a-uuid", "SBER", 60, 0, 1000}),
               core::SnapshotNotFound);
  EXPECT_TRUE(f.calls->empty());
}

TEST(ClickHouseBarSource, UnknownSecurity) {
  Fixture f;
  f.securityCount = "0\n";
  auto source = f.make();
  EXPECT_THROW(source.load({kSnap, "NOPE", 60, 0, 1000}),
               core::SecurityNotFound);
}

TEST(ClickHouseBarSource, EmptyRangeIsEmptyNotError) {
  Fixture f;
  auto source = f.make();
  EXPECT_TRUE(source.load({kSnap, "SBER", 60, 1000, 1000}).empty());
  f.rows = "";
  EXPECT_TRUE(source.load({kSnap, "SBER", 60, 0, 1000}).empty());
}

TEST(ClickHouseBarSource, IntervalTooLargeForTradeAggregation) {
  Fixture f;
  f.candleCount = "0\n";
  auto source = f.make();
  EXPECT_THROW(source.load({kSnap, "SBER", 604800, 0, 1000}),
               core::MarketDataError);
  EXPECT_THROW(source.load({kSnap, "SBER", 0, 0, 1000}), std::invalid_argument);
}

TEST(ClickHouseBarSource, RejectsBadServerData) {
  Fixture f;
  auto source = f.make();
  f.rows = "1\t2\t1.0\t1.0\t1.0\n"; // не хватает столбцов
  EXPECT_THROW(source.load({kSnap, "SBER", 60, 0, 1000}),
               core::MarketDataError);
  f.rows = "1\t2\tabc\t1\t1\t1\t1\t1\n"; // не число
  EXPECT_THROW(source.load({kSnap, "SBER", 60, 0, 1000}),
               core::MarketDataError);
  f.rows =
      "5\t6\t1\t1\t1\t1\t1\t1\n5\t6\t1\t1\t1\t1\t1\t1\n"; // дубликат ts_open
  EXPECT_THROW(source.load({kSnap, "SBER", 60, 0, 1000}),
               core::MarketDataError);
}

TEST(ClickHouseBarSource, AmbiguousSecurityIsAnError) {
  // Один secid на двух площадках в версии данных: выбор «первой» записи
  // недетерминирован (INV-4).
  Fixture f;
  f.securityCount = "2\n";
  auto source = f.make();
  EXPECT_THROW(source.security(kSnap, "SBER"), core::MarketDataError);
}

TEST(ClickHouseBarSource, TruncatedResponseIsAnError) {
  Fixture f;
  auto source = f.make();
  f.rows =
      "1792000000000\t1792000060000\t1.0\t1.0\t1.0\t1.0\t1\t1.0"; // нет
                                                                  // завершающего
                                                                  // перевода
                                                                  // строки
  EXPECT_THROW(source.load({kSnap, "SBER", 60, 0, 1792000999000}),
               worker::ClickHouseError);
  EXPECT_THROW(worker::parseTabSeparated("a\tb"), worker::ClickHouseError);
  EXPECT_NO_THROW(worker::parseTabSeparated("a\tb\n"));
}

TEST(ClickHouseBarSource, OnlyBarsAvailableByToMs) {
  Fixture f;
  auto source = f.make();
  source.load({kSnap, "SBER", 60, 0, 456});
  EXPECT_NE(f.calls->back().sql.find(
                "available_at <= fromUnixTimestamp64Milli({to_ms:Int64}"),
            std::string::npos);
  // Защита на стороне клиента: сервер вернул бар, который к toMs ещё не
  // известен, либо available_at < ts_open.
  f.rows = "100\t999\t1\t1\t1\t1\t1\t1\n";
  EXPECT_THROW(source.load({kSnap, "SBER", 60, 0, 456}), core::MarketDataError);
  f.rows = "100\t50\t1\t1\t1\t1\t1\t1\n";
  EXPECT_THROW(source.load({kSnap, "SBER", 60, 0, 456}), core::MarketDataError);
}

TEST(ClickHouseBarSource, ParsesSecurity) {
  Fixture f;
  f.rows = "LKOH\tНефть и газ\t1\t0.500000\n";
  auto source = f.make();
  const auto sec = source.security(kSnap, "LKOH");
  EXPECT_EQ(sec.secid, "LKOH");
  EXPECT_EQ(sec.sector, "Нефть и газ");
  EXPECT_EQ(sec.lotSize, 1);
  EXPECT_EQ(sec.priceStep, Decimal::parse("0.5"));
}

} // namespace
