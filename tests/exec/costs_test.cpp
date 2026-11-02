#include <gtest/gtest.h>

#include <stdexcept>

#include "core/exec/costs.hpp"

namespace {

using core::Amounts;
using core::CostConfig;
using core::Decimal;
using core::Side;

Decimal D(const char* text) { return Decimal::parse(text); }

constexpr std::int64_t kLotA = 10;
constexpr std::int64_t kLotB = 10000;

TEST(Costs, Defaults) {
  const CostConfig c = CostConfig::defaults();
  EXPECT_EQ(c.commissionRate, D("0.0005"));
  EXPECT_EQ(c.exchangeFeeRate, D("0.0001"));
  EXPECT_EQ(c.spread, D("0.001"));
  EXPECT_EQ(c.slippageKappa, D("0.1"));
  ASSERT_TRUE(c.liquidityShare.has_value());
  EXPECT_EQ(*c.liquidityShare, D("0.10"));
}

TEST(Costs, ZeroHasNoCostsAndNoLimit) {
  const CostConfig c = CostConfig::zero();
  EXPECT_TRUE(c.commissionRate.isZero());
  EXPECT_TRUE(c.exchangeFeeRate.isZero());
  EXPECT_TRUE(c.spread.isZero());
  EXPECT_TRUE(c.slippageKappa.isZero());
  EXPECT_FALSE(c.liquidityShare.has_value());
}

TEST(Costs, FillPriceBuyA) {
  // frac = 0.1 * 100 / 50000 = 0.0002; m = 1.0007; raw = 271.539945.
  const CostConfig c = CostConfig::defaults();
  const Decimal price = core::fillPrice(Side::Buy, D("271.35"), 100, 50000, c, D("0.01"));
  EXPECT_EQ(price, D("271.54"));
  const Amounts a = core::amounts(price, 100, kLotA, c);
  EXPECT_EQ(a.value, D("271540.00"));
  EXPECT_EQ(a.commission, D("135.77"));
  EXPECT_EQ(a.fee, D("27.16"));
}

TEST(Costs, FillPriceBuyB) {
  // frac = 0.1 * 300 / 90000 = 0.000333333; raw = 0.024520417.
  const CostConfig c = CostConfig::defaults();
  const Decimal price = core::fillPrice(Side::Buy, D("0.0245"), 300, 90000, c, D("0.000005"));
  EXPECT_EQ(price, D("0.024525"));
  const Amounts a = core::amounts(price, 300, kLotB, c);
  EXPECT_EQ(a.value, D("73575.00"));
  EXPECT_EQ(a.commission, D("36.79"));
  EXPECT_EQ(a.fee, D("7.36"));
}

TEST(Costs, FillPriceSellA) {
  // frac = 0.003; m = 0.9965; raw = 274.0375.
  const CostConfig c = CostConfig::defaults();
  const Decimal price = core::fillPrice(Side::Sell, D("275.00"), 30, 1000, c, D("0.01"));
  EXPECT_EQ(price, D("274.03"));
  const Amounts a = core::amounts(price, 30, kLotA, c);
  EXPECT_EQ(a.value, D("82209.00"));
  EXPECT_EQ(a.commission, D("41.11"));
  EXPECT_EQ(a.fee, D("8.23"));
}

TEST(Costs, FillPriceUsesOneDirectedModeForBothRoundings) {
  // s = 2e-9 -> s/2 = 1e-9; kappa = 0. Точное base * m = 100.0000001 * 0.999999999 = 99.9999999999999999.
  // Продажа: FLOOR до 9 знаков (99.999999999), затем FLOOR до шага -> 99.99.
  // Округление HALF_UP до 9 знаков дало бы 100.000000000 и цену 100.00 (выше точной).
  CostConfig c = CostConfig::zero();
  c.spread = D("0.000000002");
  const Decimal price = core::fillPrice(Side::Sell, D("100.0000001"), 1, 100, c, D("0.01"));
  EXPECT_EQ(price, D("99.99"));
}

TEST(Costs, FillPriceNotInFavorOfTrader) {
  const CostConfig c = CostConfig::defaults();
  const Decimal base = D("271.35");
  EXPECT_GE(core::fillPrice(Side::Buy, base, 1, 50000, c, D("0.01")), base);
  EXPECT_LE(core::fillPrice(Side::Sell, base, 1, 50000, c, D("0.01")), base);
}

TEST(Costs, FillPriceRejectsNonPositiveLots) {
  const CostConfig c = CostConfig::defaults();
  EXPECT_THROW(core::fillPrice(Side::Buy, D("100"), 0, 100, c, D("0.01")), std::invalid_argument);
  EXPECT_THROW(core::fillPrice(Side::Sell, D("100"), -1, 100, c, D("0.01")), std::invalid_argument);
}

TEST(Costs, NoSlippageWithoutVolumeOrKappa) {
  // V = 0: проскальзывание 0, остаётся спред: 100 * 1.0005 = 100.05 и 100 * 0.9995 = 99.95.
  const CostConfig c = CostConfig::defaults();
  EXPECT_EQ(core::fillPrice(Side::Buy, D("100"), 5, 0, c, D("0.01")), D("100.05"));
  EXPECT_EQ(core::fillPrice(Side::Sell, D("100"), 5, 0, c, D("0.01")), D("99.95"));
  // κ = 0: от объёма цена не зависит, V = 0 не мешает.
  CostConfig noKappa = c;
  noKappa.slippageKappa = D("0");
  EXPECT_EQ(core::fillPrice(Side::Buy, D("100"), 500, 0, noKappa, D("0.01")), D("100.05"));
  EXPECT_EQ(core::fillPrice(Side::Buy, D("100"), 500, 10, noKappa, D("0.01")), D("100.05"));
}

TEST(Costs, SellPriceIsClampedToOnePriceStep) {
  // q = 10·V: frac = 0.1 * 100 / 10 = 1, m = 1 - 0.0005 - 1 < 0 - цена не уходит в минус.
  const CostConfig c = CostConfig::defaults();
  EXPECT_EQ(core::fillPrice(Side::Sell, D("100"), 100, 10, c, D("0.01")), D("0.01"));
  EXPECT_EQ(core::fillPrice(Side::Sell, D("0.0245"), 100, 10, c, D("0.000005")), D("0.000005"));
  // Обычная продажа не затронута.
  EXPECT_EQ(core::fillPrice(Side::Sell, D("275.00"), 30, 1000, c, D("0.01")), D("274.03"));
}

TEST(Costs, CostSplitBuyA) {
  // spread = 271.35 * 0.0005 * 1000 = 135.675 -> 135.68; полная разница цены 0.19 * 1000 = 190.00.
  const CostConfig c = CostConfig::defaults();
  const core::CostSplit s = core::costSplit(D("271.35"), D("271.54"), 100, kLotA, c);
  EXPECT_EQ(s.spreadCost, D("135.68"));
  EXPECT_EQ(s.slippageCost, D("54.32"));
}

TEST(Costs, CostSplitSellUsesAbsoluteDifference) {
  // sell: base 275.00, price 274.03: разница 0.97 * 300 = 291.00; spread = 275 * 0.0005 * 300 = 41.25.
  const CostConfig c = CostConfig::defaults();
  const core::CostSplit s = core::costSplit(D("275.00"), D("274.03"), 30, kLotA, c);
  EXPECT_EQ(s.spreadCost, D("41.25"));
  EXPECT_EQ(s.slippageCost, D("249.75"));
}

}  // namespace
