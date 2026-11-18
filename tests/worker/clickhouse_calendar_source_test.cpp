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
  std::string Sql;
  QueryParams Params;
};

class FakeClient final : public worker::ClickHouseClient {
public:
  using HandlerFn = std::function<std::string(const std::string &Sql)>;
  FakeClient(std::shared_ptr<std::vector<Call>> CallLog, HandlerFn Handler)
      : Calls(std::move(CallLog)), Respond(std::move(Handler)) {}
  std::string query(const std::string &Sql,
                    const QueryParams &Params) override {
    Calls->push_back({Sql, Params});
    return Respond(Sql);
  }

private:
  std::shared_ptr<std::vector<Call>> Calls;
  HandlerFn Respond;
};

struct Fixture {
  std::shared_ptr<std::vector<Call>> Calls =
      std::make_shared<std::vector<Call>>();
  std::string ReadySnapshots = "1\n";
  std::string CalendarRows;
  std::string CandleCount = "5\n";
  std::string BarRows;

  std::unique_ptr<worker::ClickHouseClient> client() {
    return std::make_unique<FakeClient>(
        Calls, [this](const std::string &Sql) -> std::string {
          if (Sql.find("FROM snapshots_ready") != std::string::npos)
            return ReadySnapshots;
          if (Sql.find("FROM trading_calendar") != std::string::npos)
            return CalendarRows;
          if (Sql.find("SELECT count() FROM securities") != std::string::npos)
            return "1\n";
          if (Sql.find("SELECT count() FROM candles") != std::string::npos)
            return CandleCount;
          return BarRows;
        });
  }
  worker::ClickHouseCalendarSource calendar() {
    return worker::ClickHouseCalendarSource(client());
  }
  bool usedSql(const std::string &Part) const {
    for (const auto &C : *Calls) {
      if (C.Sql.find(Part) != std::string::npos)
        return true;
    }
    return false;
  }
  std::string param(const std::string &Name) const {
    for (const auto &[n, v] : Calls->back().Params) {
      if (n == Name)
        return v;
    }
    return "<нет>";
  }
};

TEST(ClickHouseCalendarSource, ParsesTabSeparatedDays) {
  Fixture F;
  F.CalendarRows = "2026-10-19\t1\n2026-10-24\t0\n2026-10-26\t1\n";
  auto Source = F.calendar();
  const auto Days = Source.tradingDays(kSnap, 20261019, 20261031);
  const std::vector<TradingDay> Expected{
      {20261019, true}, {20261024, false}, {20261026, true}};
  EXPECT_EQ(Days, Expected);
}

TEST(ClickHouseCalendarSource, RangeAndSnapshotGoAsParameters) {
  Fixture F;
  auto Source = F.calendar();
  EXPECT_TRUE(Source.tradingDays(kSnap, 20261001, 20261130).empty());
  EXPECT_EQ(F.param("sid"), kSnap);
  EXPECT_EQ(F.param("f"), "2026-10-01");
  EXPECT_EQ(F.param("t"), "2026-11-30");
  const std::string &Sql = F.Calls->back().Sql;
  EXPECT_NE(Sql.find("{sid:UUID}"), std::string::npos);
  EXPECT_NE(Sql.find("BETWEEN {f:Date} AND {t:Date}"), std::string::npos);
  EXPECT_NE(Sql.find("ORDER BY date"),
            std::string::npos); // INV-4: порядок задан явно
  EXPECT_NE(Sql.find("FORMAT TabSeparated"), std::string::npos);
}

TEST(ClickHouseCalendarSource, SnapshotMustBeReady) {
  Fixture F;
  F.ReadySnapshots = "0\n";
  auto Source = F.calendar();
  EXPECT_THROW(Source.tradingDays(kSnap, 20261001, 20261031),
               core::SnapshotNotFound);
  EXPECT_FALSE(F.usedSql("FROM trading_calendar"));
  EXPECT_TRUE(F.usedSql("snapshots_ready"));
}

TEST(ClickHouseCalendarSource, MalformedSnapshotIdNeverReachesServer) {
  Fixture F;
  auto Source = F.calendar();
  EXPECT_THROW(Source.tradingDays("not-a-uuid", 20261001, 20261031),
               core::SnapshotNotFound);
  EXPECT_TRUE(F.Calls->empty());
}

TEST(ClickHouseCalendarSource, EmptyOrInvertedRange) {
  Fixture F;
  auto Source = F.calendar();
  EXPECT_TRUE(Source.tradingDays(kSnap, 20261031, 20261001).empty());
  EXPECT_FALSE(F.usedSql("FROM trading_calendar"));
  F.ReadySnapshots = "0\n";
  EXPECT_THROW(Source.tradingDays(kSnap, 20261031, 20261001),
               core::SnapshotNotFound);
}

TEST(ClickHouseCalendarSource, InvalidRequestDate) {
  Fixture F;
  auto Source = F.calendar();
  EXPECT_THROW(Source.tradingDays(kSnap, 20261301, 20261331),
               std::invalid_argument);
  EXPECT_THROW(Source.tradingDays(kSnap, 20250229, 20261031),
               std::invalid_argument);
  EXPECT_NO_THROW(Source.tradingDays(kSnap, 20240229, 20261031));
  EXPECT_TRUE(F.Calls->size() <= 2u);
}

TEST(ClickHouseCalendarSource, RejectsBadServerData) {
  Fixture F;
  auto Source = F.calendar();
  F.CalendarRows = "2026-10-19\n"; // не хватает столбца
  EXPECT_THROW(Source.tradingDays(kSnap, 20261001, 20261031),
               core::MarketDataError);
  F.CalendarRows = "19.10.2026\t1\n"; // чужой формат даты
  EXPECT_THROW(Source.tradingDays(kSnap, 20261001, 20261031),
               core::MarketDataError);
  F.CalendarRows = "2026-10-19\t2\n"; // is_trading не 0/1
  EXPECT_THROW(Source.tradingDays(kSnap, 20261001, 20261031),
               core::MarketDataError);
  F.CalendarRows = "2026-10-19\t1\n2026-10-19\t1\n"; // дубликат даты
  EXPECT_THROW(Source.tradingDays(kSnap, 20261001, 20261031),
               core::MarketDataError);
  F.CalendarRows = "2026-10-20\t1\n2026-10-19\t1\n"; // не по возрастанию
  EXPECT_THROW(Source.tradingDays(kSnap, 20261001, 20261031),
               core::MarketDataError);
  F.CalendarRows = "2026-09-01\t1\n"; // вне диапазона
  EXPECT_THROW(Source.tradingDays(kSnap, 20261001, 20261031),
               core::MarketDataError);
  F.CalendarRows = "2026-10-19\t1"; // оборванный ответ
  EXPECT_THROW(Source.tradingDays(kSnap, 20261001, 20261031),
               worker::ClickHouseError);
}

TEST(ClickHouseCalendarSource, TransportErrorPropagates) {
  struct Failing final : worker::ClickHouseClient {
    std::string query(const std::string &, const QueryParams &) override {
      throw worker::ClickHouseError("нет связи");
    }
  };
  worker::ClickHouseCalendarSource Source(std::make_unique<Failing>());
  EXPECT_THROW(Source.tradingDays(kSnap, 20261001, 20261031),
               worker::ClickHouseError);
  EXPECT_THROW(worker::ClickHouseCalendarSource(
                   std::unique_ptr<worker::ClickHouseClient>()),
               std::invalid_argument);
}

// Интервала шага нет среди свечей версии — бары агрегируются из минутных свечей.
TEST(ClickHouseBarSourceTradesFallback, MissingIntervalReadsCandlesFromMinutes) {
  Fixture F;
  F.CandleCount = "0\n";
  F.BarRows = "1792000000000\t1792000060000\t100.000000\t101.500000\t99."
              "500000\t99.500000\t42\t4224.000000\n";
  worker::ClickHouseBarSource Bars(F.client());
  const auto Loaded = Bars.load({kSnap, "SBER", 300, 0, 2000000000000});
  ASSERT_EQ(Loaded.size(), 1u);
  EXPECT_EQ(Loaded[0].Volume, 42);
  EXPECT_TRUE(F.usedSql("interval_sec = 60"));
  // Интервал есть среди свечей — агрегацию не вызываем.
  Fixture G;
  worker::ClickHouseBarSource Stored(G.client());
  Stored.load({kSnap, "SBER", 300, 0, 2000000000000});
  EXPECT_FALSE(G.usedSql("interval_sec = 60"));
}

} // namespace
