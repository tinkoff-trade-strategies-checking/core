#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp"

using core::Decimal;
using core::Portfolio;
using core::Side;

namespace {

Decimal D(const char* text) { return Decimal::parse(text); }

// Покупка/продажа с готовыми суммами (как их считает исполнение).
void buy(Portfolio& p, std::int64_t lots, const char* price, const char* value, const char* commission,
         const char* fee, std::int64_t step = 1, std::int64_t orderId = 0, int instrument = 0) {
  p.applyFill(instrument, Side::Buy, lots, D(price), D(value), D(commission), D(fee), step, orderId);
}
void sell(Portfolio& p, std::int64_t lots, const char* price, const char* value, const char* commission,
          const char* fee, std::int64_t step = 2, std::int64_t orderId = 2, int instrument = 0) {
  p.applyFill(instrument, Side::Sell, lots, D(price), D(value), D(commission), D(fee), step, orderId);
}

TEST(Portfolio, BuyThenSellRoundTrip) {
  Portfolio p(D("1000000"), 1);
  buy(p, 100, "271.54", "271540.00", "135.77", "27.16", 3, 4);
  EXPECT_EQ(p.cash(), D("728297.07"));
  EXPECT_EQ(p.lots(0), 100);

  sell(p, 100, "274.03", "274030.00", "137.02", "27.41", 5, 6);
  EXPECT_EQ(p.cash(), D("1002162.64"));
  EXPECT_EQ(p.lots(0), 0);

  ASSERT_EQ(p.roundTrips().size(), 1u);
  const auto& t = p.roundTrips()[0];
  EXPECT_EQ(t.instrument, 0);
  EXPECT_EQ(t.entryStep, 3);
  EXPECT_EQ(t.exitStep, 5);
  EXPECT_EQ(t.entryOrderId, 4);
  EXPECT_EQ(t.lots, 100);
  EXPECT_EQ(t.entryValue, D("271540.00"));
  EXPECT_EQ(t.exitValue, D("274030.00"));
  EXPECT_EQ(t.costs, D("327.36"));
  EXPECT_EQ(t.netPnl, D("2162.64"));
}

TEST(Portfolio, AvgPriceWeighted) {
  Portfolio p(D("1000000"), 1);
  EXPECT_EQ(p.avgPrice(0), Decimal::zero());
  buy(p, 100, "271.54", "271540.00", "135.77", "27.16");
  buy(p, 50, "268.91", "134455.00", "67.23", "13.45");
  EXPECT_EQ(p.avgPrice(0), D("270.663333333"));
  EXPECT_EQ(p.lots(0), 150);
}

TEST(Portfolio, AvgPriceUnchangedBySellAndResetAtZero) {
  Portfolio p(D("1000000"), 1);
  buy(p, 100, "271.54", "271540.00", "135.77", "27.16");
  sell(p, 40, "280", "112000.00", "56.00", "11.20");
  EXPECT_EQ(p.avgPrice(0), D("271.54"));
  EXPECT_EQ(p.lots(0), 60);
  sell(p, 60, "280", "168000.00", "84.00", "16.80");
  EXPECT_EQ(p.avgPrice(0), Decimal::zero());
  // новая позиция считается с нуля
  buy(p, 10, "100", "1000.00", "0.50", "0.10");
  EXPECT_EQ(p.avgPrice(0), D("100"));
}

TEST(Portfolio, PositionValueHalfUp) {
  EXPECT_EQ(Portfolio::positionValue(3, 10, D("0.0245")), D("0.74"));  // 0.735 -> 0.74
  EXPECT_EQ(Portfolio::positionValue(0, 10, D("5")), Decimal::zero());
  EXPECT_EQ(Portfolio::positionValue(100, 10, D("271.54")), D("271540.00"));
}

TEST(Portfolio, SellMoreThanPositionThrows) {
  Portfolio p(D("1000000"), 1);
  EXPECT_THROW(sell(p, 1, "10", "10.00", "0.01", "0.01"), std::logic_error);
  buy(p, 5, "10", "50.00", "0.03", "0.01");
  const Decimal before = p.cash();
  EXPECT_THROW(sell(p, 6, "10", "60.00", "0.03", "0.01"), std::logic_error);
  EXPECT_EQ(p.cash(), before);  // состояние не меняется
  EXPECT_EQ(p.lots(0), 5);
  EXPECT_TRUE(p.roundTrips().empty());
}

TEST(Portfolio, FifoPartialSellsSplitProportionally) {
  Portfolio p(D("1000000"), 1);
  buy(p, 10, "100", "1000.00", "0.50", "0.10", 1, 0);  // издержки 0.60
  buy(p, 10, "110", "1100.00", "0.55", "0.11", 2, 2);  // издержки 0.66
  // Продажа 15 лотов: 10 из первой покупки, 5 из второй.
  sell(p, 15, "120", "1800.00", "0.90", "0.18", 3, 4);  // издержки 1.08
  ASSERT_EQ(p.roundTrips().size(), 2u);
  const auto& a = p.roundTrips()[0];
  EXPECT_EQ(a.entryOrderId, 0);
  EXPECT_EQ(a.entryStep, 1);
  EXPECT_EQ(a.lots, 10);
  EXPECT_EQ(a.entryValue, D("1000.00"));
  EXPECT_EQ(a.exitValue, D("1200.00"));  // 1800 * 10 / 15
  EXPECT_EQ(a.costs, D("1.32"));         // 0.60 + 1.08 * 10 / 15 = 0.60 + 0.72
  EXPECT_EQ(a.netPnl, D("198.68"));
  const auto& b = p.roundTrips()[1];
  EXPECT_EQ(b.entryOrderId, 2);
  EXPECT_EQ(b.lots, 5);
  EXPECT_EQ(b.entryValue, D("550.00"));  // 1100 * 5 / 10
  EXPECT_EQ(b.exitValue, D("600.00"));
  EXPECT_EQ(b.costs, D("0.69"));         // 0.66 * 5 / 10 = 0.33 + 1.08 * 5 / 15 = 0.36
  EXPECT_EQ(b.netPnl, D("49.31"));
  EXPECT_EQ(p.lots(0), 5);

  // Остаток второй покупки закрывается следующей продажей: база пропорции - исходные 10 лотов.
  sell(p, 5, "120", "600.00", "0.30", "0.06", 4, 6);
  ASSERT_EQ(p.roundTrips().size(), 3u);
  const auto& c = p.roundTrips()[2];
  EXPECT_EQ(c.entryOrderId, 2);
  EXPECT_EQ(c.entryValue, D("550.00"));
  EXPECT_EQ(c.exitValue, D("600.00"));
  EXPECT_EQ(c.costs, D("0.69"));  // 0.33 + 0.36
  EXPECT_EQ(p.lots(0), 0);
}

TEST(Portfolio, InstrumentsAreIndependent) {
  Portfolio p(D("1000"), 2);
  buy(p, 2, "100", "200.00", "0.10", "0.02", 1, 0, 0);
  EXPECT_EQ(p.lots(1), 0);
  EXPECT_THROW(sell(p, 1, "100", "100.00", "0.05", "0.01", 2, 2, 1), std::logic_error);
  EXPECT_THROW(p.lots(2), std::out_of_range);
  EXPECT_THROW(p.lots(-1), std::out_of_range);
}

TEST(Portfolio, EquityIsCashPlusPositions) {
  Portfolio p(D("1000000"), 2);
  EXPECT_EQ(p.equity({std::nullopt, std::nullopt}, {10, 1}), D("1000000"));  // позиций нет - цены не нужны
  buy(p, 100, "271.54", "271540.00", "135.77", "27.16");
  buy(p, 3, "0.0245", "0.74", "0.01", "0.01", 1, 2, 1);
  const Decimal cash = p.cash();
  const Decimal eq = p.equity({D("274.03"), D("0.0245")}, {10, 10});
  EXPECT_EQ(eq, cash + D("274030.00") + D("0.74"));
  EXPECT_THROW(p.equity({std::nullopt, D("1")}, {10, 10}), std::logic_error);  // позиция без цены
  EXPECT_THROW(p.equity({D("1")}, {10}), std::invalid_argument);
}

}  // namespace
