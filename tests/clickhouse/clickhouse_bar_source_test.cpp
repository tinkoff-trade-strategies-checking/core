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
  std::string Sql;
  QueryParams Params;
};

// Подменный клиент: отвечает по подстроке SQL, запоминает все вызовы.
class FakeClient final : public worker::ClickHouseClient {
public:
  using HandlerFn =
      std::function<std::string(const std::string &Sql, const QueryParams &)>;
  FakeClient(std::shared_ptr<std::vector<Call>> CallLog, HandlerFn Handler)
      : Calls(std::move(CallLog)), Respond(std::move(Handler)) {}
  std::string query(const std::string &Sql,
                    const QueryParams &Params) override {
    Calls->push_back({Sql, Params});
    return Respond(Sql, Params);
  }

private:
  std::shared_ptr<std::vector<Call>> Calls;
  HandlerFn Respond;
};

struct Fixture {
  std::shared_ptr<std::vector<Call>> Calls =
      std::make_shared<std::vector<Call>>();
  std::string ReadySnapshots = "1\n";
  std::string SecurityCount = "1\n";
  std::string CandleCount = "5\n";
  std::string Rows;

  worker::ClickHouseBarSource make() {
    return worker::ClickHouseBarSource(std::make_unique<FakeClient>(
        Calls,
        [this](const std::string &Sql, const QueryParams &) -> std::string {
          if (Sql.find("FROM snapshots_ready") != std::string::npos)
            return ReadySnapshots;
          if (Sql.find("SELECT count() FROM securities") != std::string::npos)
            return SecurityCount;
          if (Sql.find("SELECT count() FROM candles") != std::string::npos)
            return CandleCount;
          return Rows;
        }));
  }
  bool usedSql(const std::string &Part) const {
    for (const auto &C : *Calls) {
      if (C.Sql.find(Part) != std::string::npos)
        return true;
    }
    return false;
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

TEST(ClickHouseBarSource, FallsBackToTradesWhenIntervalNotStored) {
  Fixture F;
  F.CandleCount = "0\n";
  F.Rows = "1792000000000\t1792000060000\t100.000000\t101.500000\t99."
           "500000\t99.500000\t42\t4224.000000\n";
  auto Source = F.make();
  const auto Bars = Source.load({kSnap, "SYNTH", 60, 0, 2000000000000});
  ASSERT_EQ(Bars.size(), 1u);
  EXPECT_EQ(Bars[0].Volume, 42);
  EXPECT_TRUE(F.usedSql("candles_from_trades"));
}

TEST(ClickHouseBarSource, NoSqlInjectionThroughSecid) {
  Fixture F;
  auto Source = F.make();
  const std::string Evil = "X' OR 1=1; DROP TABLE candles --";
  Source.load({kSnap, Evil, 60, 0, 1000});
  for (const auto &Call : *F.Calls) {
    EXPECT_EQ(Call.Sql.find("DROP"), std::string::npos) << Call.Sql;
    EXPECT_EQ(Call.Sql.find("OR 1=1"), std::string::npos) << Call.Sql;
  }
  bool PassedAsParam = false;
  for (const auto &Call : *F.Calls) {
    for (const auto &[name, value] : Call.Params) {
      if (name == "secid" && value == Evil)
        PassedAsParam = true;
    }
  }
  EXPECT_TRUE(PassedAsParam);
}

TEST(ClickHouseBarSource, RangeAndIntervalGoAsParameters) {
  Fixture F;
  auto Source = F.make();
  Source.load({kSnap, "SBER", 60, 123, 456});
  const Call &Last = F.Calls->back();
  bool From = false, To = false, Interval = false;
  for (const auto &[name, value] : Last.Params) {
    if (name == "from_ms" && value == "123")
      From = true;
    if (name == "to_ms" && value == "456")
      To = true;
    if (name == "interval_sec" && value == "60")
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
  F.SecurityCount = "0\n";
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

TEST(ClickHouseBarSource, IntervalTooLargeForTradeAggregation) {
  Fixture F;
  F.CandleCount = "0\n";
  auto Source = F.make();
  EXPECT_THROW(Source.load({kSnap, "SBER", 604800, 0, 1000}),
               core::MarketDataError);
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
  F.SecurityCount = "2\n";
  auto Source = F.make();
  EXPECT_THROW(Source.security(kSnap, "SBER"), core::MarketDataError);
}

TEST(ClickHouseBarSource, AmbiguousSecurityInLoadIsAnError) {
  // Неоднозначный инструмент в load() должен отклоняться так же, как и в
  // security() (INV-4).
  Fixture F;
  F.SecurityCount = "2\n";
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
  EXPECT_THROW(Source.load({kSnap, "SBER", 60, 0, 456}), core::MarketDataError);
  F.Rows = "100\t50\t1\t1\t1\t1\t1\t1\n";
  EXPECT_THROW(Source.load({kSnap, "SBER", 60, 0, 456}), core::MarketDataError);
}

TEST(ClickHouseBarSource, ParsesSecurity) {
  Fixture F;
  F.Rows = "LKOH\tНефть и газ\t1\t0.500000\n";
  auto Source = F.make();
  const auto Sec = Source.security(kSnap, "LKOH");
  EXPECT_EQ(Sec.Secid, "LKOH");
  EXPECT_EQ(Sec.Sector, "Нефть и газ");
  EXPECT_EQ(Sec.LotSize, 1);
  EXPECT_EQ(Sec.PriceStep, Decimal::parse("0.5"));
}

} // namespace
