#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/market/bar_cursor.hpp"
#include "core/market/in_memory_bar_source.hpp"

namespace {

using core::Bar;
using core::BarRequest;
using core::Decimal;
using core::InMemoryBarSource;
using core::Security;

constexpr const char* kSnap = "snap-1";

Bar makeBar(std::int64_t tsOpenMs, const char* close) {
  Bar b;
  b.tsOpenMs = tsOpenMs;
  b.availableAtMs = tsOpenMs + 60000;
  b.open = b.high = b.low = b.close = Decimal::parse(close);
  b.volume = 1;
  b.value = Decimal::parse(close);
  return b;
}

InMemoryBarSource makeSource() {
  InMemoryBarSource source;
  source.addSecurity(kSnap, Security{"SBER", "Финансы", 10, Decimal::parse("0.01")});
  // Добавляем вразнобой: источник обязан отсортировать по tsOpenMs.
  source.addSeries(kSnap, "SBER", 60,
                   {makeBar(180000, "3"), makeBar(0, "1"), makeBar(120000, "2.5"), makeBar(60000, "1.5")});
  return source;
}

TEST(InMemoryBarSource, LoadsSortedWithZeroBasedIndex) {
  auto source = makeSource();
  const auto bars = source.load({kSnap, "SBER", 60, 0, 1000000});
  ASSERT_EQ(bars.size(), 4u);
  for (std::size_t i = 0; i < bars.size(); ++i) {
    EXPECT_EQ(bars[i].index, static_cast<std::int64_t>(i));
    if (i > 0) {
      EXPECT_LT(bars[i - 1].tsOpenMs, bars[i].tsOpenMs);
    }
  }
  EXPECT_EQ(bars[2].close, Decimal::parse("2.5"));
}

TEST(InMemoryBarSource, RangeIsHalfOpenAndReindexed) {
  auto source = makeSource();
  const auto bars = source.load({kSnap, "SBER", 60, 60000, 180000});
  ASSERT_EQ(bars.size(), 2u);
  EXPECT_EQ(bars[0].tsOpenMs, 60000);
  EXPECT_EQ(bars[0].index, 0);
  EXPECT_EQ(bars[1].tsOpenMs, 120000);
  EXPECT_EQ(bars[1].index, 1);
}

// INV-1: бар, который к toMs ещё не известен (availableAtMs > toMs), в результат не попадает.
TEST(InMemoryBarSource, ExcludesBarsNotYetAvailableByToMs) {
  auto source = makeSource();  // бар i доступен в tsOpenMs + 60000
  EXPECT_EQ(source.load({kSnap, "SBER", 60, 0, 120000}).size(), 2u);    // бары 0 и 60000 известны к 120000
  EXPECT_EQ(source.load({kSnap, "SBER", 60, 0, 119999}).size(), 1u);    // бар 60000 ещё не известен (доступен в 120000)
  EXPECT_EQ(source.load({kSnap, "SBER", 60, 0, 60000}).size(), 1u);     // только бар 0 (доступен в 60000)
  EXPECT_EQ(source.load({kSnap, "SBER", 60, 0, 59999}).size(), 0u);
}

TEST(InMemoryBarSource, ResultFeedsBarCursor) {
  auto source = makeSource();
  core::BarCursor cursor(source.load({kSnap, "SBER", 60, 60000, 1000000}));
  int steps = 0;
  while (cursor.advance()) {
    ++steps;
    EXPECT_EQ(cursor.visible().size(), static_cast<std::size_t>(steps));
  }
  EXPECT_EQ(steps, 3);
}

TEST(InMemoryBarSource, OrderOfAdditionDoesNotMatter) {
  InMemoryBarSource a;
  InMemoryBarSource b;
  const std::vector<Bar> forward{makeBar(0, "1"), makeBar(60000, "2"), makeBar(120000, "3")};
  std::vector<Bar> backward(forward.rbegin(), forward.rend());
  a.addSeries(kSnap, "X", 60, forward);
  b.addSeries(kSnap, "X", 60, backward);
  a.addSecurity(kSnap, Security{"X", "", 1, Decimal::one()});
  b.addSecurity(kSnap, Security{"X", "", 1, Decimal::one()});
  EXPECT_EQ(a.load({kSnap, "X", 60, 0, 1000000}), b.load({kSnap, "X", 60, 0, 1000000}));
}

TEST(InMemoryBarSource, EmptySeriesForKnownSecurity) {
  auto source = makeSource();
  EXPECT_TRUE(source.load({kSnap, "SBER", 60, 500000, 600000}).empty());   // вне диапазона
  EXPECT_TRUE(source.load({kSnap, "SBER", 3600, 0, 1000000}).empty());     // нет такого интервала
}

TEST(InMemoryBarSource, UnknownSnapshotAndSecurityThrow) {
  auto source = makeSource();
  EXPECT_THROW(source.load({"nope", "SBER", 60, 0, 1}), core::SnapshotNotFound);
  EXPECT_THROW(source.load({kSnap, "GAZP", 60, 0, 1}), core::SecurityNotFound);
  EXPECT_THROW(source.security("nope", "SBER"), core::SnapshotNotFound);
  EXPECT_THROW(source.security(kSnap, "GAZP"), core::SecurityNotFound);
}

TEST(InMemoryBarSource, SecurityLookup) {
  auto source = makeSource();
  const Security sec = source.security(kSnap, "SBER");
  EXPECT_EQ(sec.sector, "Финансы");
  EXPECT_EQ(sec.lotSize, 10);
  EXPECT_EQ(sec.priceStep, Decimal::parse("0.01"));
}

TEST(InMemoryBarSource, DuplicateTimestampRejected) {
  InMemoryBarSource source;
  EXPECT_THROW(source.addSeries(kSnap, "X", 60, {makeBar(0, "1"), makeBar(0, "2")}), std::invalid_argument);
}

}  // namespace
