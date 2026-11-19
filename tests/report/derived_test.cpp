#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/report/derived.hpp"
#include "support/fixture1.hpp"

// core#54: производные проценты для экранов.

namespace core {
namespace {

Decimal d(const char *Text) { return Decimal::parse(Text); }

Timeline fixtureTimeline() {
  auto F = testing::fixture1();
  return Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                        Timeframe::D1, 20240109, 20240122);
}

Fill fillAt(std::int64_t Step, const char *Commission) {
  Fill F;
  F.Step = Step;
  F.Commission = d(Commission);
  return F;
}

PortfolioSnapshot snapshot(const char *Cash, const char *Equity,
                           const char *PositionValue) {
  PortfolioSnapshot S;
  S.Cash = d(Cash);
  S.Equity = d(Equity);
  S.Lots = {5};
  S.AvgPrice = {d("100")};
  S.MarketValue = {d(PositionValue)};
  return S;
}

TEST(Derived, PortfolioPcts) {
  const PortfolioPcts P = portfolioPcts(snapshot("500000", "1010000", "510000"),
                                        d("1000000"), d("135.77"));
  EXPECT_EQ(P.InvestedPct, d("50.50")); // 510000 / 1010000 = 50.495 -> 50.50
  EXPECT_EQ(P.CostsPct, d("0.01"));     // 0.013577 -> 0.01
  EXPECT_EQ(P.PnlPct, d("1.00"));
}

TEST(Derived, PortfolioPctsEdgeCases) {
  // без позиций и издержек
  const PortfolioPcts Flat =
      portfolioPcts(snapshot("1000000", "1000000", "0"), d("1000000"), d("0"));
  EXPECT_EQ(Flat.InvestedPct, Decimal::zero());
  EXPECT_EQ(Flat.CostsPct, Decimal::zero());
  EXPECT_EQ(Flat.PnlPct, Decimal::zero());
  // банкротство: equity 0 - доля вложенного 0, P&L -100
  const PortfolioPcts Bust =
      portfolioPcts(snapshot("0", "0", "0"), d("1000000"), d("0"));
  EXPECT_EQ(Bust.InvestedPct, Decimal::zero());
  EXPECT_EQ(Bust.PnlPct, d("-100.00"));
  // убыток
  EXPECT_EQ(
      portfolioPcts(snapshot("0", "900000", "900000"), d("1000000"), d("0"))
          .PnlPct,
      d("-10.00"));
  EXPECT_THROW(portfolioPcts(snapshot("0", "0", "0"), d("0"), d("0")),
               std::invalid_argument);
}

TEST(Derived, CumulativeCostsUpToStep) {
  const std::vector<Fill> Fills = {fillAt(1, "10.00"), fillAt(3, "5.50"),
                                   fillAt(3, "0.25"), fillAt(7, "100.00")};
  EXPECT_EQ(cumulativeCosts({}, 9), Decimal::zero());
  EXPECT_EQ(cumulativeCosts(Fills, 0), Decimal::zero());
  EXPECT_EQ(cumulativeCosts(Fills, 1), d("10.00"));
  EXPECT_EQ(cumulativeCosts(Fills, 3), d("15.75"));
  EXPECT_EQ(cumulativeCosts(Fills, 9), d("115.75"));
}

TEST(Derived, PositionPcts) {
  EXPECT_EQ(positionSharePct(d("510000"), d("1010000")), d("50.50"));
  EXPECT_EQ(positionSharePct(d("1"), d("0")), Decimal::zero());
  // 5 лотов по 10 шт. по 100 = 5000; стоимость 5300 -> +6.00%
  EXPECT_EQ(positionPnlPct(d("5300"), d("100"), 5, 10), d("6.00"));
  EXPECT_EQ(positionPnlPct(d("4750"), d("100"), 5, 10), d("-5.00"));
  EXPECT_EQ(positionPnlPct(d("0"), d("0"), 0, 10), Decimal::zero());
}

TEST(Derived, InstrumentChange) {
  const Timeline T = fixtureTimeline();
  // Бумага A, шаг 0: нет предыдущего бара - к open (open = close).
  const InstrumentChange A0 = instrumentChange(T, 0, 0);
  ASSERT_TRUE(A0.StepPct && A0.PeriodPct);
  EXPECT_EQ(*A0.StepPct, Decimal::zero());
  EXPECT_EQ(*A0.PeriodPct, Decimal::zero());
  // Шаг 3: close 106 к 103 = 2.91%; к open первого бара 100 = 6.00%.
  const InstrumentChange A3 = instrumentChange(T, 0, 3);
  EXPECT_EQ(*A3.StepPct, d("2.91"));
  EXPECT_EQ(*A3.PeriodPct, d("6.00"));
  // Падение: шаг 4 (104 к 106 = -1.89%), период +4.00%.
  const InstrumentChange A4 = instrumentChange(T, 0, 4);
  EXPECT_EQ(*A4.StepPct, d("-1.89"));
  EXPECT_EQ(*A4.PeriodPct, d("4.00"));
  // Бумага B без бара на шаге 3: берётся последний бар <= курсора, цена
  // постоянна.
  const InstrumentChange B3 = instrumentChange(T, 1, 3);
  ASSERT_TRUE(B3.StepPct && B3.PeriodPct);
  EXPECT_EQ(*B3.StepPct, Decimal::zero());
  EXPECT_EQ(*B3.PeriodPct, Decimal::zero());
}

// INV-1: значение на курсоре не зависит от баров позже курсора.
TEST(Derived, InstrumentChangeIgnoresFuture) {
  const Timeline T = fixtureTimeline();
  const InstrumentChange At4 = instrumentChange(T, 0, 4);
  const InstrumentChange At9 = instrumentChange(T, 0, 9);
  EXPECT_NE(*At4.PeriodPct, *At9.PeriodPct);
  EXPECT_EQ(*At9.PeriodPct, d("8.00"));
  EXPECT_EQ(*instrumentChange(T, 0, 4).StepPct, *At4.StepPct);
}

} // namespace
} // namespace core
