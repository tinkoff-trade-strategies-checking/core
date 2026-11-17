#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/market/in_memory_bar_source.hpp"

namespace {

using core::Bar;
using core::BarRequest;
using core::Decimal;
using core::InMemoryBarSource;
using core::Security;

constexpr const char *kSnap = "snap-1";

Bar makeBar(std::int64_t TsOpenMs, const char *Close) {
  Bar B;
  B.TsOpenMs = TsOpenMs;
  B.AvailableAtMs = TsOpenMs + 60000;
  B.Open = B.High = B.Low = B.Close = Decimal::parse(Close);
  B.Volume = 1;
  B.Value = Decimal::parse(Close);
  return B;
}

InMemoryBarSource makeSource() {
  InMemoryBarSource Source;
  Source.addSecurity(kSnap,
                     Security{"SBER", "Финансы", 10, Decimal::parse("0.01")});
  // Добавляем вразнобой: источник обязан отсортировать по tsOpenMs.
  Source.addSeries(kSnap, "SBER", 60,
                   {makeBar(180000, "3"), makeBar(0, "1"),
                    makeBar(120000, "2.5"), makeBar(60000, "1.5")});
  return Source;
}

TEST(InMemoryBarSource, LoadsSortedWithZeroBasedIndex) {
  auto Source = makeSource();
  const auto Bars = Source.load({kSnap, "SBER", 60, 0, 1000000});
  ASSERT_EQ(Bars.size(), 4u);
  for (std::size_t I = 0; I < Bars.size(); ++I) {
    EXPECT_EQ(Bars[I].Index, static_cast<std::int64_t>(I));
    if (I > 0) {
      EXPECT_LT(Bars[I - 1].TsOpenMs, Bars[I].TsOpenMs);
    }
  }
  EXPECT_EQ(Bars[2].Close, Decimal::parse("2.5"));
}

TEST(InMemoryBarSource, RangeIsHalfOpenAndReindexed) {
  auto Source = makeSource();
  const auto Bars = Source.load({kSnap, "SBER", 60, 60000, 180000});
  ASSERT_EQ(Bars.size(), 2u);
  EXPECT_EQ(Bars[0].TsOpenMs, 60000);
  EXPECT_EQ(Bars[0].Index, 0);
  EXPECT_EQ(Bars[1].TsOpenMs, 120000);
  EXPECT_EQ(Bars[1].Index, 1);
}

// INV-1: бар, который к toMs ещё не известен (availableAtMs > toMs), в
// результат не попадает.
TEST(InMemoryBarSource, ExcludesBarsNotYetAvailableByToMs) {
  auto Source = makeSource(); // бар i доступен в tsOpenMs + 60000
  EXPECT_EQ(Source.load({kSnap, "SBER", 60, 0, 120000}).size(),
            2u); // бары 0 и 60000 известны к 120000
  EXPECT_EQ(Source.load({kSnap, "SBER", 60, 0, 119999}).size(),
            1u); // бар 60000 ещё не известен (доступен в 120000)
  EXPECT_EQ(Source.load({kSnap, "SBER", 60, 0, 60000}).size(),
            1u); // только бар 0 (доступен в 60000)
  EXPECT_EQ(Source.load({kSnap, "SBER", 60, 0, 59999}).size(), 0u);
}

TEST(InMemoryBarSource, OrderOfAdditionDoesNotMatter) {
  InMemoryBarSource A;
  InMemoryBarSource B;
  const std::vector<Bar> Forward{makeBar(0, "1"), makeBar(60000, "2"),
                                 makeBar(120000, "3")};
  std::vector<Bar> Backward(Forward.rbegin(), Forward.rend());
  A.addSeries(kSnap, "X", 60, Forward);
  B.addSeries(kSnap, "X", 60, Backward);
  A.addSecurity(kSnap, Security{"X", "", 1, Decimal::one()});
  B.addSecurity(kSnap, Security{"X", "", 1, Decimal::one()});
  EXPECT_EQ(A.load({kSnap, "X", 60, 0, 1000000}),
            B.load({kSnap, "X", 60, 0, 1000000}));
}

TEST(InMemoryBarSource, EmptySeriesForKnownSecurity) {
  auto Source = makeSource();
  EXPECT_TRUE(Source.load({kSnap, "SBER", 60, 500000, 600000})
                  .empty()); // вне диапазона
  EXPECT_TRUE(Source.load({kSnap, "SBER", 3600, 0, 1000000})
                  .empty()); // нет такого интервала
}

TEST(InMemoryBarSource, UnknownSnapshotAndSecurityThrow) {
  auto Source = makeSource();
  EXPECT_THROW(Source.load({"nope", "SBER", 60, 0, 1}), core::SnapshotNotFound);
  EXPECT_THROW(Source.load({kSnap, "GAZP", 60, 0, 1}), core::SecurityNotFound);
  EXPECT_THROW(Source.security("nope", "SBER"), core::SnapshotNotFound);
  EXPECT_THROW(Source.security(kSnap, "GAZP"), core::SecurityNotFound);
}

TEST(InMemoryBarSource, SecurityLookup) {
  auto Source = makeSource();
  const Security Sec = Source.security(kSnap, "SBER");
  EXPECT_EQ(Sec.Sector, "Финансы");
  EXPECT_EQ(Sec.LotSize, 10);
  EXPECT_EQ(Sec.PriceStep, Decimal::parse("0.01"));
}

TEST(InMemoryBarSource, DuplicateTimestampRejected) {
  InMemoryBarSource Source;
  EXPECT_THROW(
      Source.addSeries(kSnap, "X", 60, {makeBar(0, "1"), makeBar(0, "2")}),
      std::invalid_argument);
}

} // namespace
