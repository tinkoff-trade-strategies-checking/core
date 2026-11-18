#include "clickhouse/clickhouse_ruonia_source.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/market/bar_source.hpp"
#include "core/market/ruonia_source.hpp"

namespace {

using core::DailyRate;
using core::Decimal;
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

struct FakeRuoniaClient {
  std::shared_ptr<std::vector<Call>> Calls =
      std::make_shared<std::vector<Call>>();
  std::string ReadySnapshots = "1\n";
  std::string RuoniaRows;

  std::unique_ptr<worker::ClickHouseClient> client() {
    return std::make_unique<FakeClient>(
        Calls, [this](const std::string &Sql) -> std::string {
          if (Sql.find("FROM snapshots_ready") != std::string::npos)
            return ReadySnapshots;
          if (Sql.find("FROM ruonia") != std::string::npos)
            return RuoniaRows;
          return "";
        });
  }
};

TEST(ClickHouseRuoniaSource, RejectsInvalidUuid) {
  FakeRuoniaClient F;
  worker::ClickHouseRuoniaSource Source(F.client());
  EXPECT_THROW((void)Source.ruoniaRates("not-a-uuid", 20240115),
               core::SnapshotNotFound);
}

TEST(ClickHouseRuoniaSource, ThrowsWhenSnapshotNotReady) {
  FakeRuoniaClient F;
  F.ReadySnapshots = "0\n";
  worker::ClickHouseRuoniaSource Source(F.client());
  EXPECT_THROW((void)Source.ruoniaRates(kSnap, 20240115),
               core::SnapshotNotFound);
}

TEST(ClickHouseRuoniaSource, ParsesRates) {
  FakeRuoniaClient F;
  F.RuoniaRows = "2024-01-09\t15.25\n"
                 "2024-01-10\t15.30\n";
  worker::ClickHouseRuoniaSource Source(F.client());
  const std::vector<DailyRate> Rates = Source.ruoniaRates(kSnap, 20240115);
  ASSERT_EQ(Rates.size(), 2u);
  EXPECT_EQ(Rates[0].Yyyymmdd, 20240109);
  EXPECT_EQ(Rates[0].Rate, Decimal::parse("15.25"));
  EXPECT_EQ(Rates[1].Yyyymmdd, 20240110);
  EXPECT_EQ(Rates[1].Rate, Decimal::parse("15.30"));
}

TEST(ClickHouseRuoniaSource, UnorderedDatesThrowsMarketDataError) {
  FakeRuoniaClient F;
  F.RuoniaRows = "2024-01-10\t15.30\n"
                 "2024-01-09\t15.25\n";
  worker::ClickHouseRuoniaSource Source(F.client());
  EXPECT_THROW((void)Source.ruoniaRates(kSnap, 20240115),
               core::MarketDataError);
}

TEST(ClickHouseRuoniaSource, BadRateThrowsMarketDataError) {
  FakeRuoniaClient F;
  F.RuoniaRows = "2024-01-09\tabc\n";
  worker::ClickHouseRuoniaSource Source(F.client());
  EXPECT_THROW((void)Source.ruoniaRates(kSnap, 20240115),
               core::MarketDataError);
}

TEST(ClickHouseRuoniaSource, ZeroRateThrowsMarketDataError) {
  FakeRuoniaClient F;
  F.RuoniaRows = "2024-01-09\t0\n";
  worker::ClickHouseRuoniaSource Source(F.client());
  EXPECT_THROW((void)Source.ruoniaRates(kSnap, 20240115),
               core::MarketDataError);

  F.RuoniaRows = "2024-01-09\t0.00\n";
  EXPECT_THROW((void)Source.ruoniaRates(kSnap, 20240115),
               core::MarketDataError);
}

TEST(ClickHouseRuoniaSource, NegativeRateThrowsMarketDataError) {
  FakeRuoniaClient F;
  F.RuoniaRows = "2024-01-09\t-1.5\n";
  worker::ClickHouseRuoniaSource Source(F.client());
  EXPECT_THROW((void)Source.ruoniaRates(kSnap, 20240115),
               core::MarketDataError);
}

TEST(ClickHouseRuoniaSource, BadDateThrowsMarketDataError) {
  FakeRuoniaClient F;
  F.RuoniaRows = "2024-13-40\t15.25\n";
  worker::ClickHouseRuoniaSource Source(F.client());
  EXPECT_THROW((void)Source.ruoniaRates(kSnap, 20240115),
               core::MarketDataError);
}

TEST(ClickHouseRuoniaSource, DateAfterUpToThrowsMarketDataError) {
  FakeRuoniaClient F;
  F.RuoniaRows = "2024-01-09\t15.25\n"
                 "2024-01-16\t15.30\n";
  worker::ClickHouseRuoniaSource Source(F.client());
  EXPECT_THROW((void)Source.ruoniaRates(kSnap, 20240115),
               core::MarketDataError);
}

TEST(ClickHouseRuoniaSource, PassesUpToDateParameter) {
  FakeRuoniaClient F;
  F.RuoniaRows = "2024-01-09\t15.25\n";
  worker::ClickHouseRuoniaSource Source(F.client());
  (void)Source.ruoniaRates(kSnap, 20240115);
  bool Found = false;
  for (const Call &C : *F.Calls) {
    if (C.Sql.find("FROM ruonia") == std::string::npos)
      continue;
    for (const auto &[Name, Value] : C.Params) {
      if (Name == "t") {
        Found = true;
        EXPECT_EQ(Value, "2024-01-15");
      }
    }
  }
  EXPECT_TRUE(Found);
}

TEST(InMemoryRuoniaSource, SortsAndFiltersUpTo) {
  core::InMemoryRuoniaSource Source;
  Source.add("snap1", {
                          DailyRate{20240115, Decimal::parse("16.0")},
                          DailyRate{20240109, Decimal::parse("15.0")},
                      });

  const auto Rates = Source.ruoniaRates("snap1", 20240110);
  ASSERT_EQ(Rates.size(), 1u);
  EXPECT_EQ(Rates[0].Yyyymmdd, 20240109);
  EXPECT_EQ(Rates[0].Rate, Decimal::parse("15.0"));

  const auto All = Source.ruoniaRates("snap1", 20240120);
  ASSERT_EQ(All.size(), 2u);
  EXPECT_EQ(All[0].Yyyymmdd, 20240109);
  EXPECT_EQ(All[1].Yyyymmdd, 20240115);
}

TEST(InMemoryRuoniaSource, DuplicateDateThrowsInvalidArgument) {
  core::InMemoryRuoniaSource Source;
  EXPECT_THROW(Source.add("snap1",
                          {
                              DailyRate{20240109, Decimal::parse("15.0")},
                              DailyRate{20240109, Decimal::parse("15.2")},
                          }),
               std::invalid_argument);
}

TEST(InMemoryRuoniaSource, MissingSnapshotThrowsSnapshotNotFound) {
  core::InMemoryRuoniaSource Source;
  EXPECT_THROW((void)Source.ruoniaRates("snap1", 20240110),
               core::SnapshotNotFound);
}

} // namespace
