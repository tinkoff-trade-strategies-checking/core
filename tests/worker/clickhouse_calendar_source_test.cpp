// Юнит-тесты ClickHouseCalendarSource без сервера: клиент подменён записанными
// ответами. Проверка на настоящем ClickHouse - в интеграционных тестах (нужен
// CLICKHOUSE_URL).
#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "clickhouse/clickhouse_bar_source.hpp"
#include "clickhouse/clickhouse_calendar_source.hpp"

namespace {

using core::TradingDay;
using worker::QueryParams;

constexpr const char *kSnap = "ec6b3c97-7036-56cf-ba4d-ec4e919e682d";

struct Call {
  std::string sql;
  QueryParams params;
};

class FakeClient final : public worker::ClickHouseClient {
public:
  using Handler = std::function<std::string(const std::string &sql)>;
  FakeClient(std::shared_ptr<std::vector<Call>> calls, Handler handler)
      : calls_(std::move(calls)), handler_(std::move(handler)) {}
  std::string query(const std::string &sql,
                    const QueryParams &params) override {
    calls_->push_back({sql, params});
    return handler_(sql);
  }

private:
  std::shared_ptr<std::vector<Call>> calls_;
  Handler handler_;
};

struct Fixture {
  std::shared_ptr<std::vector<Call>> calls =
      std::make_shared<std::vector<Call>>();
  std::string readySnapshots = "1\n";
  std::string calendarRows;
  std::string candleCount = "5\n";
  std::string barRows;

  std::unique_ptr<worker::ClickHouseClient> client() {
    return std::make_unique<FakeClient>(
        calls, [this](const std::string &sql) -> std::string {
          if (sql.find("FROM snapshots_ready") != std::string::npos)
            return readySnapshots;
          if (sql.find("FROM trading_calendar") != std::string::npos)
            return calendarRows;
          if (sql.find("SELECT count() FROM securities") != std::string::npos)
            return "1\n";
          if (sql.find("SELECT count() FROM candles") != std::string::npos)
            return candleCount;
          return barRows;
        });
  }
  worker::ClickHouseCalendarSource calendar() {
    return worker::ClickHouseCalendarSource(client());
  }
  bool usedSql(const std::string &part) const {
    for (const auto &c : *calls) {
      if (c.sql.find(part) != std::string::npos)
        return true;
    }
    return false;
  }
  std::string param(const std::string &name) const {
    for (const auto &[n, v] : calls->back().params) {
      if (n == name)
        return v;
    }
    return "<нет>";
  }
};

TEST(ClickHouseCalendarSource, ParsesTabSeparatedDays) {
  Fixture f;
  f.calendarRows = "2026-10-19\t1\n2026-10-24\t0\n2026-10-26\t1\n";
  auto source = f.calendar();
  const auto days = source.tradingDays(kSnap, 20261019, 20261031);
  const std::vector<TradingDay> expected{
      {20261019, true}, {20261024, false}, {20261026, true}};
  EXPECT_EQ(days, expected);
}

TEST(ClickHouseCalendarSource, RangeAndSnapshotGoAsParameters) {
  Fixture f;
  auto source = f.calendar();
  EXPECT_TRUE(source.tradingDays(kSnap, 20261001, 20261130).empty());
  EXPECT_EQ(f.param("sid"), kSnap);
  EXPECT_EQ(f.param("f"), "2026-10-01");
  EXPECT_EQ(f.param("t"), "2026-11-30");
  const std::string &sql = f.calls->back().sql;
  EXPECT_NE(sql.find("{sid:UUID}"), std::string::npos);
  EXPECT_NE(sql.find("BETWEEN {f:Date} AND {t:Date}"), std::string::npos);
  EXPECT_NE(sql.find("ORDER BY date"),
            std::string::npos); // INV-4: порядок задан явно
  EXPECT_NE(sql.find("FORMAT TabSeparated"), std::string::npos);
}

TEST(ClickHouseCalendarSource, SnapshotMustBeReady) {
  Fixture f;
  f.readySnapshots = "0\n";
  auto source = f.calendar();
  EXPECT_THROW(source.tradingDays(kSnap, 20261001, 20261031),
               core::SnapshotNotFound);
  EXPECT_FALSE(f.usedSql("FROM trading_calendar"));
  EXPECT_TRUE(f.usedSql("snapshots_ready"));
}

TEST(ClickHouseCalendarSource, MalformedSnapshotIdNeverReachesServer) {
  Fixture f;
  auto source = f.calendar();
  EXPECT_THROW(source.tradingDays("not-a-uuid", 20261001, 20261031),
               core::SnapshotNotFound);
  EXPECT_TRUE(f.calls->empty());
}

TEST(ClickHouseCalendarSource, EmptyOrInvertedRange) {
  Fixture f;
  auto source = f.calendar();
  EXPECT_TRUE(source.tradingDays(kSnap, 20261031, 20261001).empty());
  EXPECT_FALSE(f.usedSql("FROM trading_calendar"));
  f.readySnapshots = "0\n";
  EXPECT_THROW(source.tradingDays(kSnap, 20261031, 20261001),
               core::SnapshotNotFound);
}

TEST(ClickHouseCalendarSource, InvalidRequestDate) {
  Fixture f;
  auto source = f.calendar();
  EXPECT_THROW(source.tradingDays(kSnap, 20261301, 20261331),
               std::invalid_argument);
  EXPECT_THROW(source.tradingDays(kSnap, 20250229, 20261031),
               std::invalid_argument);
  EXPECT_NO_THROW(source.tradingDays(kSnap, 20240229, 20261031));
  EXPECT_TRUE(f.calls->size() <= 2u);
}

TEST(ClickHouseCalendarSource, RejectsBadServerData) {
  Fixture f;
  auto source = f.calendar();
  f.calendarRows = "2026-10-19\n"; // не хватает столбца
  EXPECT_THROW(source.tradingDays(kSnap, 20261001, 20261031),
               core::MarketDataError);
  f.calendarRows = "19.10.2026\t1\n"; // чужой формат даты
  EXPECT_THROW(source.tradingDays(kSnap, 20261001, 20261031),
               core::MarketDataError);
  f.calendarRows = "2026-10-19\t2\n"; // is_trading не 0/1
  EXPECT_THROW(source.tradingDays(kSnap, 20261001, 20261031),
               core::MarketDataError);
  f.calendarRows = "2026-10-19\t1\n2026-10-19\t1\n"; // дубликат даты
  EXPECT_THROW(source.tradingDays(kSnap, 20261001, 20261031),
               core::MarketDataError);
  f.calendarRows = "2026-10-20\t1\n2026-10-19\t1\n"; // не по возрастанию
  EXPECT_THROW(source.tradingDays(kSnap, 20261001, 20261031),
               core::MarketDataError);
  f.calendarRows = "2026-09-01\t1\n"; // вне диапазона
  EXPECT_THROW(source.tradingDays(kSnap, 20261001, 20261031),
               core::MarketDataError);
  f.calendarRows = "2026-10-19\t1"; // оборванный ответ
  EXPECT_THROW(source.tradingDays(kSnap, 20261001, 20261031),
               worker::ClickHouseError);
}

TEST(ClickHouseCalendarSource, TransportErrorPropagates) {
  struct Failing final : worker::ClickHouseClient {
    std::string query(const std::string &, const QueryParams &) override {
      throw worker::ClickHouseError("нет связи");
    }
  };
  worker::ClickHouseCalendarSource source(std::make_unique<Failing>());
  EXPECT_THROW(source.tradingDays(kSnap, 20261001, 20261031),
               worker::ClickHouseError);
  EXPECT_THROW(worker::ClickHouseCalendarSource(
                   std::unique_ptr<worker::ClickHouseClient>()),
               std::invalid_argument);
}

// Пункт 2 задачи: интервала шага нет среди свечей версии - бары читаются из
// candles_from_trades.
TEST(ClickHouseBarSourceTradesFallback, MissingIntervalReadsCandlesFromTrades) {
  Fixture f;
  f.candleCount = "0\n";
  f.barRows = "1792000000000\t1792000060000\t100.000000\t101.500000\t99."
              "500000\t99.500000\t42\t4224.000000\n";
  worker::ClickHouseBarSource bars(f.client());
  const auto loaded = bars.load({kSnap, "SBER", 60, 0, 2000000000000});
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].volume, 42);
  EXPECT_TRUE(f.usedSql("candles_from_trades(snapshot_id = {snapshot_id:UUID}, "
                        "interval_sec = {interval_sec:UInt32})"));
  // Интервал есть среди свечей - из сделок не читаем.
  Fixture g;
  worker::ClickHouseBarSource stored(g.client());
  stored.load({kSnap, "SBER", 60, 0, 2000000000000});
  EXPECT_FALSE(g.usedSql("candles_from_trades"));
}

} // namespace
