#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/market/bar.hpp"
#include "core/market/calendar_source.hpp"
#include "core/market/instrument.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp"
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
                           const char *PositionValue,
                           const char *CostBasis = "0") {
  PortfolioSnapshot S;
  S.Cash = d(Cash);
  S.Equity = d(Equity);
  S.Lots = {5};
  S.AvgPrice = {d("100")};
  S.MarketValue = {d(PositionValue)};
  S.CostBasis = {d(CostBasis)};
  return S;
}

// База всех процентов - начальный капитал: cash + invested = equity.
TEST(Derived, PortfolioPctsBaseIsInitialCapital) {
  // убыток 20% при полностью вложенном портфеле: invested не 100%, а 80%
  const PortfolioPcts Full =
      portfolioPcts(snapshot("0", "800000", "800000"), d("1000000"), d("0"));
  EXPECT_EQ(Full.InvestedPct, d("80.00"));
  EXPECT_EQ(Full.CashPct, d("0.00"));
  EXPECT_EQ(Full.EquityPct, d("80.00"));
  EXPECT_EQ(Full.PnlPct, d("-20.00"));
  EXPECT_EQ(Full.Invested, d("800000"));
  EXPECT_EQ(Full.Pnl, d("-200000"));

  const PortfolioPcts P = portfolioPcts(snapshot("500000", "1010000", "510000"),
                                        d("1000000"), d("135.77"));
  EXPECT_EQ(P.InvestedPct, d("51.00"));
  EXPECT_EQ(P.CashPct, d("50.00"));
  EXPECT_EQ(P.EquityPct, d("101.00"));
  EXPECT_EQ(P.CashPct + P.InvestedPct, P.EquityPct);
  EXPECT_EQ(P.CostsPct, d("0.01")); // 0.013577 -> 0.01
  EXPECT_EQ(P.Costs, d("135.77"));
  EXPECT_EQ(P.PnlPct, d("1.00"));
}

// Равенство точное и при дробных долях: invested = equity - cash.
TEST(Derived, PortfolioPctsSumIsExact) {
  const PortfolioPcts P = portfolioPcts(
      snapshot("333333.33", "666666.67", "333333.34"), d("1000000"), d("0"));
  EXPECT_EQ(P.CashPct + P.InvestedPct, P.EquityPct);
  EXPECT_EQ(P.EquityPct, d("66.67"));
  EXPECT_EQ(P.CashPct, d("33.33"));
  EXPECT_EQ(P.InvestedPct, d("33.34"));
}

TEST(Derived, PortfolioPctsEdgeCases) {
  const PortfolioPcts Flat =
      portfolioPcts(snapshot("1000000", "1000000", "0"), d("1000000"), d("0"));
  EXPECT_EQ(Flat.InvestedPct, Decimal::zero());
  EXPECT_EQ(Flat.CostsPct, Decimal::zero());
  EXPECT_EQ(Flat.PnlPct, Decimal::zero());
  EXPECT_EQ(Flat.EquityPct, d("100.00"));
  // банкротство: equity 0
  const PortfolioPcts Bust =
      portfolioPcts(snapshot("0", "0", "0"), d("1000000"), d("0"));
  EXPECT_EQ(Bust.InvestedPct, Decimal::zero());
  EXPECT_EQ(Bust.EquityPct, Decimal::zero());
  EXPECT_EQ(Bust.PnlPct, d("-100.00"));
  EXPECT_THROW(portfolioPcts(snapshot("0", "0", "0"), d("0"), d("0")),
               std::invalid_argument);
}

TEST(Derived, CostLedgerUpToStep) {
  const std::vector<Fill> Fills = {fillAt(1, "10.00"), fillAt(3, "5.50"),
                                   fillAt(3, "0.25"), fillAt(7, "100.00")};
  EXPECT_EQ(CostLedger({}).upTo(9), Decimal::zero());
  const CostLedger L(Fills);
  EXPECT_EQ(L.upTo(0), Decimal::zero());
  EXPECT_EQ(L.upTo(1), d("10.00"));
  EXPECT_EQ(L.upTo(2), d("10.00"));
  EXPECT_EQ(L.upTo(3), d("15.75"));
  EXPECT_EQ(L.upTo(9), d("115.75"));
}

TEST(Derived, PositionPcts) {
  EXPECT_EQ(positionValuePct(d("510000"), d("1000000")), d("51.00"));
  // стоимость 5300 при себестоимости 5000: +300 от капитала 1 000 000 = 0.03%
  EXPECT_EQ(positionPnlPct(d("5300"), d("5000"), d("1000000")), d("0.03"));
  EXPECT_EQ(positionPnlPct(d("4750"), d("5000"), d("1000000")), d("-0.03"));
  EXPECT_EQ(positionPnlPct(d("0"), d("0"), d("1000000")), Decimal::zero());
}

// Себестоимость - точная сумма покупок открытых лотов, а не avg * лоты.
TEST(Derived, PortfolioCostBasisIsExactSumOfOpenLots) {
  Portfolio P(d("1000000"), 1);
  // 3 лота по 10 шт.: цены 100.0000001 и 100.0000002 дают разную себестоимость
  // при округлённом avg; value уже округлено до копеек (сумма сделки).
  P.applyFill(0, Side::Buy, 2, d("100.01"), d("2000.20"), d("1"), d("0"), 1, 0);
  P.applyFill(0, Side::Buy, 1, d("100.03"), d("1000.30"), d("1"), d("0"), 2, 1);
  EXPECT_EQ(P.costBasis(0), d("3000.50"));
  // продажа 1 лота гасит первую покупку на половину: остаётся 1000.10 + 1000.30
  P.applyFill(0, Side::Sell, 1, d("100.05"), d("1000.50"), d("1"), d("0"), 3,
              2);
  EXPECT_EQ(P.costBasis(0), d("2000.40"));
  P.applyFill(0, Side::Sell, 2, d("100.05"), d("2001.00"), d("1"), d("0"), 4,
              3);
  EXPECT_EQ(P.costBasis(0), Decimal::zero());
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
  // Бумага B без бара на шаге 3: за шаг изменения нет, за период - по
  // последнему бару до курсора.
  const InstrumentChange B3 = instrumentChange(T, 1, 3);
  EXPECT_FALSE(B3.StepPct.has_value());
  ASSERT_TRUE(B3.PeriodPct.has_value());
  EXPECT_EQ(*B3.PeriodPct, Decimal::zero());
}

// Предыдущий close берётся из прогрева, если на шагах сессии бара ещё не было
// (а не open этого же бара).
TEST(Derived, InstrumentChangePrevCloseFromWarmup) {
  std::vector<Instrument> Instruments = {{"A", "TKA", 10, d("0.01")}};
  const std::vector<TradingDay> Calendar = {{20250110, true}, {20250113, true}};
  auto MakeBar = [](std::int64_t Ts, const char *Open, const char *High,
                    const char *Low, const char *Close) {
    Bar B;
    B.TsOpenMs = Ts;
    B.AvailableAtMs = Ts + 60000;
    B.Open = Decimal::parse(Open);
    B.High = Decimal::parse(High);
    B.Low = Decimal::parse(Low);
    B.Close = Decimal::parse(Close);
    B.Volume = 10;
    return B;
  };
  const std::int64_t HistStart = 1736492400000LL; // 2025-01-10 10:00 MSK
  const std::int64_t SessStart = 1736751600000LL; // 2025-01-13 10:00 MSK
  const Timeline Hist = Timeline::build(
      Instruments,
      {{MakeBar(HistStart, "180", "195", "175", "190"),
        MakeBar(HistStart + 60000, "190", "205", "185", "200")}},
      Calendar);
  const Timeline T =
      Timeline::build(Instruments,
                      {{MakeBar(SessStart, "210", "225", "205", "220")}},
                      Calendar)
          .withWarmup(Hist, 2);
  const InstrumentChange C = instrumentChange(T, 0, 0);
  ASSERT_TRUE(C.StepPct && C.PeriodPct);
  EXPECT_EQ(*C.StepPct, d("10.00")); // 220 к close прогрева 200, не к open 210
  EXPECT_EQ(*C.PeriodPct, d("4.76")); // 220 к open первого бара 210
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
