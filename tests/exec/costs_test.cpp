#include <gtest/gtest.h>

#include <stdexcept>

#include "core/exec/costs.hpp"

namespace {

using core::Amounts;
using core::CostConfig;
using core::Decimal;
using core::Side;

Decimal d(const char *Text) { return Decimal::parse(Text); }

constexpr std::int64_t kLotA = 10;
constexpr std::int64_t kLotB = 10000;

TEST(Costs, Defaults) {
  const CostConfig C = CostConfig::defaults();
  EXPECT_EQ(C.CommissionRate, d("0.003"));
  EXPECT_EQ(C.ExchangeFeeRate, d("0.0001"));
  EXPECT_EQ(C.Spread, d("0.001"));
  EXPECT_EQ(C.SlippageKappa, d("0.1"));
  ASSERT_TRUE(C.LiquidityShare.has_value());
  EXPECT_EQ(*C.LiquidityShare, d("0.10"));
}

TEST(Costs, ZeroHasNoCostsAndNoLimit) {
  const CostConfig C = CostConfig::zero();
  EXPECT_TRUE(C.CommissionRate.isZero());
  EXPECT_TRUE(C.ExchangeFeeRate.isZero());
  EXPECT_TRUE(C.Spread.isZero());
  EXPECT_TRUE(C.SlippageKappa.isZero());
  EXPECT_FALSE(C.LiquidityShare.has_value());
}

TEST(Costs, FillPriceBuyA) {
  // frac = 0.1 * 100 / 50000 = 0.0002; m = 1.0007; raw = 271.539945.
  const CostConfig C = CostConfig::defaults();
  const Decimal Price =
      core::fillPrice(Side::Buy, d("271.35"), 100, 50000, C, d("0.01"));
  EXPECT_EQ(Price, d("271.54"));
  const Amounts A = core::amounts(Price, 100, kLotA, C);
  EXPECT_EQ(A.Value, d("271540.00"));
  EXPECT_EQ(A.Commission, d("814.62"));
  EXPECT_EQ(A.Fee, d("27.16"));
}

TEST(Costs, FillPriceBuyB) {
  // frac = 0.1 * 300 / 90000 = 0.000333333; raw = 0.024520417.
  const CostConfig C = CostConfig::defaults();
  const Decimal Price =
      core::fillPrice(Side::Buy, d("0.0245"), 300, 90000, C, d("0.000005"));
  EXPECT_EQ(Price, d("0.024525"));
  const Amounts A = core::amounts(Price, 300, kLotB, C);
  EXPECT_EQ(A.Value, d("73575.00"));
  EXPECT_EQ(A.Commission, d("220.73"));
  EXPECT_EQ(A.Fee, d("7.36"));
}

TEST(Costs, FillPriceSellA) {
  // frac = 0.003; m = 0.9965; raw = 274.0375.
  const CostConfig C = CostConfig::defaults();
  const Decimal Price =
      core::fillPrice(Side::Sell, d("275.00"), 30, 1000, C, d("0.01"));
  EXPECT_EQ(Price, d("274.03"));
  const Amounts A = core::amounts(Price, 30, kLotA, C);
  EXPECT_EQ(A.Value, d("82209.00"));
  EXPECT_EQ(A.Commission, d("246.63"));
  EXPECT_EQ(A.Fee, d("8.23"));
}

TEST(Costs, FillPriceUsesOneDirectedModeForBothRoundings) {
  // s = 2e-9 -> s/2 = 1e-9; kappa = 0. Точное base * m = 100.0000001 *
  // 0.999999999 = 99.9999999999999999. Продажа: FLOOR до 9 знаков
  // (99.999999999), затем FLOOR до шага -> 99.99. Округление HALF_UP до 9
  // знаков дало бы 100.000000000 и цену 100.00 (выше точной).
  CostConfig C = CostConfig::zero();
  C.Spread = d("0.000000002");
  const Decimal Price =
      core::fillPrice(Side::Sell, d("100.0000001"), 1, 100, C, d("0.01"));
  EXPECT_EQ(Price, d("99.99"));
}

TEST(Costs, FillPriceNotInFavorOfTrader) {
  const CostConfig C = CostConfig::defaults();
  const Decimal Base = d("271.35");
  EXPECT_GE(core::fillPrice(Side::Buy, Base, 1, 50000, C, d("0.01")), Base);
  EXPECT_LE(core::fillPrice(Side::Sell, Base, 1, 50000, C, d("0.01")), Base);
}

TEST(Costs, FillPriceRejectsNonPositiveLots) {
  const CostConfig C = CostConfig::defaults();
  EXPECT_THROW(core::fillPrice(Side::Buy, d("100"), 0, 100, C, d("0.01")),
               std::invalid_argument);
  EXPECT_THROW(core::fillPrice(Side::Sell, d("100"), -1, 100, C, d("0.01")),
               std::invalid_argument);
}

TEST(Costs, NoSlippageWithoutVolumeOrKappa) {
  // V = 0: проскальзывание 0, остаётся спред: 100 * 1.0005 = 100.05 и 100 *
  // 0.9995 = 99.95.
  const CostConfig C = CostConfig::defaults();
  EXPECT_EQ(core::fillPrice(Side::Buy, d("100"), 5, 0, C, d("0.01")),
            d("100.05"));
  EXPECT_EQ(core::fillPrice(Side::Sell, d("100"), 5, 0, C, d("0.01")),
            d("99.95"));
  // κ = 0: от объёма цена не зависит, V = 0 не мешает.
  CostConfig NoKappa = C;
  NoKappa.SlippageKappa = d("0");
  EXPECT_EQ(core::fillPrice(Side::Buy, d("100"), 500, 0, NoKappa, d("0.01")),
            d("100.05"));
  EXPECT_EQ(core::fillPrice(Side::Buy, d("100"), 500, 10, NoKappa, d("0.01")),
            d("100.05"));
}

TEST(Costs, SellPriceIsClampedToOnePriceStep) {
  // q = 10·V: frac = 0.1 * 100 / 10 = 1, m = 1 - 0.0005 - 1 < 0 - цена не
  // уходит в минус.
  const CostConfig C = CostConfig::defaults();
  EXPECT_EQ(core::fillPrice(Side::Sell, d("100"), 100, 10, C, d("0.01")),
            d("0.01"));
  EXPECT_EQ(core::fillPrice(Side::Sell, d("0.0245"), 100, 10, C, d("0.000005")),
            d("0.000005"));
  // Обычная продажа не затронута.
  EXPECT_EQ(core::fillPrice(Side::Sell, d("275.00"), 30, 1000, C, d("0.01")),
            d("274.03"));
}

TEST(Costs, CostSplitBuyA) {
  // spread = 271.35 * 0.0005 * 1000 = 135.675 -> 135.68; полная разница цены
  // 0.19 * 1000 = 190.00.
  const CostConfig C = CostConfig::defaults();
  const core::CostSplit S =
      core::costSplit(d("271.35"), d("271.54"), 100, kLotA, C);
  EXPECT_EQ(S.SpreadCost, d("135.68"));
  EXPECT_EQ(S.SlippageCost, d("54.32"));
}

TEST(Costs, CostSplitSellUsesAbsoluteDifference) {
  // sell: base 275.00, price 274.03: разница 0.97 * 300 = 291.00; spread = 275
  // * 0.0005 * 300 = 41.25.
  const CostConfig C = CostConfig::defaults();
  const core::CostSplit S =
      core::costSplit(d("275.00"), d("274.03"), 30, kLotA, C);
  EXPECT_EQ(S.SpreadCost, d("41.25"));
  EXPECT_EQ(S.SlippageCost, d("249.75"));
}

} // namespace
