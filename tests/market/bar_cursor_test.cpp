#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/market/bar_cursor.hpp"

namespace {

using core::Bar;
using core::BarCursor;
using core::Decimal;

// Серия из n баров; close = 100 + index, у «будущих» баров цена-маркер не используется нигде.
std::vector<Bar> makeSeries(int n) {
  std::vector<Bar> bars;
  for (int i = 0; i < n; ++i) {
    Bar b;
    b.index = i;
    b.tsOpenMs = 1000 + i * 60000;
    b.availableAtMs = b.tsOpenMs + 60000;
    b.open = Decimal::fromUnits(100 + i);
    b.high = Decimal::fromUnits(101 + i);
    b.low = Decimal::fromUnits(99 + i);
    b.close = Decimal::fromUnits(100 + i);
    b.volume = 10 + i;
    b.value = Decimal::fromUnits(1000 + i);
    bars.push_back(b);
  }
  return bars;
}

TEST(BarCursor, StartsBeforeFirstBar) {
  BarCursor cursor(makeSeries(3));
  EXPECT_FALSE(cursor.started());
  EXPECT_TRUE(cursor.visible().empty());
  EXPECT_FALSE(cursor.currentIndex().has_value());
  EXPECT_THROW(cursor.current(), std::logic_error);
  EXPECT_TRUE(cursor.hasNext());
}

// INV-1: visible() никогда не содержит бар current + 1 (и ничего с index > current).
TEST(BarCursor, VisibleNeverIncludesNextBar) {
  constexpr int kBars = 50;
  BarCursor cursor(makeSeries(kBars));
  std::int64_t expected = -1;
  while (cursor.advance()) {
    ++expected;
    const auto cur = cursor.currentIndex();
    ASSERT_TRUE(cur.has_value());
    ASSERT_EQ(*cur, expected);
    EXPECT_EQ(cursor.current().index, expected);

    const auto visible = cursor.visible();
    ASSERT_EQ(visible.size(), static_cast<std::size_t>(expected) + 1);
    for (const Bar& bar : visible) {
      EXPECT_LE(bar.index, expected);
      EXPECT_NE(bar.index, expected + 1);
    }
    EXPECT_EQ(visible.back().index, expected);
    EXPECT_EQ(&visible.back(), &cursor.current());
  }
  EXPECT_EQ(expected, kBars - 1);
}

TEST(BarCursor, FutureBarValuesNeverLeak) {
  auto bars = makeSeries(5);
  // Бар 3 помечаем уникальным значением; пока курсор не дошёл до него, его нет в visible().
  bars[3].close = Decimal::parse("777.123456");
  BarCursor cursor(std::move(bars));
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(cursor.advance());
    for (const Bar& bar : cursor.visible()) {
      EXPECT_NE(bar.close, Decimal::parse("777.123456"));
    }
  }
  ASSERT_TRUE(cursor.advance());
  EXPECT_EQ(cursor.current().close, Decimal::parse("777.123456"));
}

TEST(BarCursor, StopsAtEndAndKeepsPosition) {
  BarCursor cursor(makeSeries(2));
  ASSERT_TRUE(cursor.advance());
  ASSERT_TRUE(cursor.advance());
  EXPECT_FALSE(cursor.hasNext());
  EXPECT_FALSE(cursor.advance());
  EXPECT_FALSE(cursor.advance());
  EXPECT_EQ(cursor.currentIndex(), 1);
  EXPECT_EQ(cursor.visible().size(), 2u);
}

TEST(BarCursor, EmptySeries) {
  BarCursor cursor(std::vector<Bar>{});
  EXPECT_FALSE(cursor.hasNext());
  EXPECT_FALSE(cursor.advance());
  EXPECT_TRUE(cursor.visible().empty());
}

TEST(BarCursor, RejectsBrokenSeries) {
  auto gap = makeSeries(3);
  gap[1].index = 5;
  EXPECT_THROW(BarCursor{gap}, std::invalid_argument);

  auto unordered = makeSeries(3);
  unordered[2].tsOpenMs = unordered[1].tsOpenMs;
  EXPECT_THROW(BarCursor{unordered}, std::invalid_argument);

  auto early = makeSeries(3);
  early[1].availableAtMs = early[1].tsOpenMs - 1;  // известен раньше начала - невозможно
  EXPECT_THROW(BarCursor{early}, std::invalid_argument);

  auto offset = makeSeries(3);
  for (auto& b : offset) ++b.index;
  EXPECT_THROW(BarCursor{offset}, std::invalid_argument);
}

}  // namespace
