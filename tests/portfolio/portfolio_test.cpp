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

Decimal d(const char *Text) { return Decimal::parse(Text); }

// Покупка/продажа с готовыми суммами (как их считает исполнение).
void buy(Portfolio &P, std::int64_t Lots, const char *Price, const char *Value,
         const char *Commission, const char *Fee, std::int64_t Step = 1,
         std::int64_t OrderId = 0, int Instrument = 0) {
  P.applyFill(Instrument, Side::Buy, Lots, d(Price), d(Value), d(Commission),
              d(Fee), Step, OrderId);
}
void sell(Portfolio &P, std::int64_t Lots, const char *Price, const char *Value,
          const char *Commission, const char *Fee, std::int64_t Step = 2,
          std::int64_t OrderId = 2, int Instrument = 0) {
  P.applyFill(Instrument, Side::Sell, Lots, d(Price), d(Value), d(Commission),
              d(Fee), Step, OrderId);
}

TEST(Portfolio, BuyThenSellRoundTrip) {
  Portfolio P(d("1000000"), 1);
  buy(P, 100, "271.54", "271540.00", "135.77", "27.16", 3, 4);
  EXPECT_EQ(P.cash(), d("728297.07"));
  EXPECT_EQ(P.lots(0), 100);

  sell(P, 100, "274.03", "274030.00", "137.02", "27.41", 5, 6);
  EXPECT_EQ(P.cash(), d("1002162.64"));
  EXPECT_EQ(P.lots(0), 0);

  ASSERT_EQ(P.roundTrips().size(), 1u);
  const auto &T = P.roundTrips()[0];
  EXPECT_EQ(T.Instrument, 0);
  EXPECT_EQ(T.EntryStep, 3);
  EXPECT_EQ(T.ExitStep, 5);
  EXPECT_EQ(T.EntryOrderId, 4);
  EXPECT_EQ(T.Lots, 100);
  EXPECT_EQ(T.EntryValue, d("271540.00"));
  EXPECT_EQ(T.ExitValue, d("274030.00"));
  EXPECT_EQ(T.Costs, d("327.36"));
  EXPECT_EQ(T.NetPnl, d("2162.64"));
}

TEST(Portfolio, AvgPriceWeighted) {
  Portfolio P(d("1000000"), 1);
  EXPECT_EQ(P.avgPrice(0), Decimal::zero());
  buy(P, 100, "271.54", "271540.00", "135.77", "27.16");
  buy(P, 50, "268.91", "134455.00", "67.23", "13.45");
  EXPECT_EQ(P.avgPrice(0), d("270.663333333"));
  EXPECT_EQ(P.lots(0), 150);
}

TEST(Portfolio, AvgPriceUnchangedBySellAndResetAtZero) {
  Portfolio P(d("1000000"), 1);
  buy(P, 100, "271.54", "271540.00", "135.77", "27.16");
  sell(P, 40, "280", "112000.00", "56.00", "11.20");
  EXPECT_EQ(P.avgPrice(0), d("271.54"));
  EXPECT_EQ(P.lots(0), 60);
  sell(P, 60, "280", "168000.00", "84.00", "16.80");
  EXPECT_EQ(P.avgPrice(0), Decimal::zero());
  // новая позиция считается с нуля
  buy(P, 10, "100", "1000.00", "0.50", "0.10");
  EXPECT_EQ(P.avgPrice(0), d("100"));
}

TEST(Portfolio, PositionValueHalfUp) {
  EXPECT_EQ(Portfolio::positionValue(3, 10, d("0.0245")),
            d("0.74")); // 0.735 -> 0.74
  EXPECT_EQ(Portfolio::positionValue(0, 10, d("5")), Decimal::zero());
  EXPECT_EQ(Portfolio::positionValue(100, 10, d("271.54")), d("271540.00"));
}

TEST(Portfolio, SellMoreThanPositionThrows) {
  Portfolio P(d("1000000"), 1);
  EXPECT_THROW(sell(P, 1, "10", "10.00", "0.01", "0.01"), std::logic_error);
  buy(P, 5, "10", "50.00", "0.03", "0.01");
  const Decimal Before = P.cash();
  EXPECT_THROW(sell(P, 6, "10", "60.00", "0.03", "0.01"), std::logic_error);
  EXPECT_EQ(P.cash(), Before); // состояние не меняется
  EXPECT_EQ(P.lots(0), 5);
  EXPECT_TRUE(P.roundTrips().empty());
}

TEST(Portfolio, FifoPartialSellsSplitProportionally) {
  Portfolio P(d("1000000"), 1);
  buy(P, 10, "100", "1000.00", "0.50", "0.10", 1, 0); // издержки 0.60
  buy(P, 10, "110", "1100.00", "0.55", "0.11", 2, 2); // издержки 0.66
  // Продажа 15 лотов: 10 из первой покупки, 5 из второй.
  sell(P, 15, "120", "1800.00", "0.90", "0.18", 3, 4); // издержки 1.08
  ASSERT_EQ(P.roundTrips().size(), 2u);
  const auto &A = P.roundTrips()[0];
  EXPECT_EQ(A.EntryOrderId, 0);
  EXPECT_EQ(A.EntryStep, 1);
  EXPECT_EQ(A.Lots, 10);
  EXPECT_EQ(A.EntryValue, d("1000.00"));
  EXPECT_EQ(A.ExitValue, d("1200.00")); // 1800 * 10 / 15
  EXPECT_EQ(A.Costs, d("1.32"));        // 0.60 + 1.08 * 10 / 15 = 0.60 + 0.72
  EXPECT_EQ(A.NetPnl, d("198.68"));
  const auto &B = P.roundTrips()[1];
  EXPECT_EQ(B.EntryOrderId, 2);
  EXPECT_EQ(B.Lots, 5);
  EXPECT_EQ(B.EntryValue, d("550.00")); // 1100 * 5 / 10
  EXPECT_EQ(B.ExitValue, d("600.00"));
  EXPECT_EQ(B.Costs, d("0.69")); // 0.66 * 5 / 10 = 0.33 + 1.08 * 5 / 15 = 0.36
  EXPECT_EQ(B.NetPnl, d("49.31"));
  EXPECT_EQ(P.lots(0), 5);

  // Остаток второй покупки закрывается следующей продажей: база пропорции -
  // исходные 10 лотов.
  sell(P, 5, "120", "600.00", "0.30", "0.06", 4, 6);
  ASSERT_EQ(P.roundTrips().size(), 3u);
  const auto &C = P.roundTrips()[2];
  EXPECT_EQ(C.EntryOrderId, 2);
  EXPECT_EQ(C.EntryValue, d("550.00"));
  EXPECT_EQ(C.ExitValue, d("600.00"));
  EXPECT_EQ(C.Costs, d("0.69")); // 0.33 + 0.36
  EXPECT_EQ(P.lots(0), 0);
}

TEST(Portfolio, InstrumentsAreIndependent) {
  Portfolio P(d("1000"), 2);
  buy(P, 2, "100", "200.00", "0.10", "0.02", 1, 0, 0);
  EXPECT_EQ(P.lots(1), 0);
  EXPECT_THROW(sell(P, 1, "100", "100.00", "0.05", "0.01", 2, 2, 1),
               std::logic_error);
  EXPECT_THROW(P.lots(2), std::out_of_range);
  EXPECT_THROW(P.lots(-1), std::out_of_range);
}

TEST(Portfolio, ApplyFillRejectsInvalidInput) {
  Portfolio P(d("1000"), 1);
  buy(P, 2, "100", "200.00", "0.10", "0.02");
  const Decimal Cash = P.cash();
  // Лоты <= 0.
  EXPECT_THROW(buy(P, 0, "100", "0.00", "0", "0"), std::invalid_argument);
  EXPECT_THROW(buy(P, -1, "100", "100.00", "0.05", "0.01"),
               std::invalid_argument);
  EXPECT_THROW(sell(P, 0, "100", "0.00", "0", "0"), std::invalid_argument);
  EXPECT_THROW(sell(P, -1, "100", "100.00", "0.05", "0.01"),
               std::invalid_argument);
  // Отрицательные цена, сумма, комиссия, сбор.
  EXPECT_THROW(buy(P, 1, "-100", "100.00", "0.05", "0.01"),
               std::invalid_argument);
  EXPECT_THROW(sell(P, 1, "-100", "100.00", "0.05", "0.01"),
               std::invalid_argument);
  EXPECT_THROW(buy(P, 1, "100", "-100.00", "0.05", "0.01"),
               std::invalid_argument);
  EXPECT_THROW(buy(P, 1, "100", "100.00", "-0.05", "0.01"),
               std::invalid_argument);
  EXPECT_THROW(sell(P, 1, "100", "100.00", "0.05", "-0.01"),
               std::invalid_argument);
  // Состояние не изменилось.
  EXPECT_EQ(P.cash(), Cash);
  EXPECT_EQ(P.lots(0), 2);
  EXPECT_TRUE(P.roundTrips().empty());
}

TEST(Portfolio, EquityIsCashPlusPositions) {
  Portfolio P(d("1000000"), 2);
  EXPECT_EQ(P.equity({std::nullopt, std::nullopt}, {10, 1}),
            d("1000000")); // позиций нет - цены не нужны
  buy(P, 100, "271.54", "271540.00", "135.77", "27.16");
  buy(P, 3, "0.0245", "0.74", "0.01", "0.01", 1, 2, 1);
  const Decimal Cash = P.cash();
  const Decimal Eq = P.equity({d("274.03"), d("0.0245")}, {10, 10});
  EXPECT_EQ(Eq, Cash + d("274030.00") + d("0.74"));
  EXPECT_THROW(P.equity({std::nullopt, d("1")}, {10, 10}),
               std::logic_error); // позиция без цены
  EXPECT_THROW(P.equity({d("1")}, {10}), std::invalid_argument);
}

} // namespace
