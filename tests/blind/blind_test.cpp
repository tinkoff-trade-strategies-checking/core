#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <vector>

#include "core/blind/blind.hpp"
#include "core/engine/session_engine.hpp"
#include "core/market/bar.hpp"
#include "core/market/calendar_source.hpp"
#include "core/market/instrument.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"

namespace {

core::Bar makeBar(std::int64_t TsMs, const char *Open, const char *High,
                  const char *Low, const char *Close, std::int64_t Vol) {
  core::Bar B;
  B.TsOpenMs = TsMs;
  B.AvailableAtMs = TsMs + 60000;
  B.Open = core::Decimal::parse(Open);
  B.High = core::Decimal::parse(High);
  B.Low = core::Decimal::parse(Low);
  B.Close = core::Decimal::parse(Close);
  B.Volume = Vol;
  return B;
}

std::vector<core::TradingDay> makeCalendar() {
  return {{20250110, true}, {20250113, true}};
}

} // namespace

TEST(BlindScale, PriceAndVolumeWithoutWarmup) {
  std::vector<core::Instrument> Instruments = {
      {"A", "TKA", 10, core::Decimal::parse("0.01")},
  };
  const std::int64_t DayStart = 1736751600000LL; // 2025-01-13 10:00 MSK
  std::vector<std::vector<core::Bar>> Bars = {{
      makeBar(DayStart, "200", "220", "190", "210", 500),
      makeBar(DayStart + 60000, "210", "230", "200", "225", 750),
  }};
  auto Tl = std::make_shared<core::Timeline>(
      core::Timeline::build(Instruments, Bars, makeCalendar()));

  const core::Decimal Cash = core::Decimal::fromUnits(100000);
  const auto Scales = core::computeBlindScales(*Tl, Cash, 1);
  ASSERT_EQ(Scales.size(), 1u);
  ASSERT_TRUE(Scales[0].P0.has_value());
  EXPECT_EQ(*Scales[0].P0, core::Decimal::fromUnits(200));
  ASSERT_TRUE(Scales[0].LotValueAtStart.has_value());
  // 200 * 10 * 100 / 100000 = 2.00
  EXPECT_EQ(*Scales[0].LotValueAtStart, core::Decimal::parse("2.00"));

  const core::BlindBar B0 = core::toBlindBar(*Tl->barOf(0, 0), Scales[0]);
  EXPECT_EQ(B0.Open, core::Decimal::parse("100.00"));
  EXPECT_EQ(B0.High, core::Decimal::parse("110.00"));
  EXPECT_EQ(B0.Low, core::Decimal::parse("95.00"));
  EXPECT_EQ(B0.Close, core::Decimal::parse("105.00"));
  EXPECT_EQ(B0.VolumeRel, core::Decimal::parse("100.00"));

  const core::BlindBar B1 = core::toBlindBar(*Tl->barOf(0, 1), Scales[0]);
  EXPECT_EQ(B1.Open, core::Decimal::parse("105.00"));
  EXPECT_EQ(B1.High, core::Decimal::parse("115.00"));
  EXPECT_EQ(B1.Low, core::Decimal::parse("100.00"));
  EXPECT_EQ(B1.Close, core::Decimal::parse("112.50"));
  EXPECT_EQ(B1.VolumeRel, core::Decimal::parse("150.00"));
}

TEST(BlindScale, PriceAndVolumeWithWarmup) {
  std::vector<core::Instrument> Instruments = {
      {"A", "TKA", 10, core::Decimal::parse("0.01")},
  };
  const std::int64_t HistStart = 1736492400000LL; // 2025-01-10 10:00 MSK
  const std::int64_t SessStart = 1736751600000LL; // 2025-01-13 10:00 MSK
  auto Hist = core::Timeline::build(
      Instruments,
      {{
          makeBar(HistStart, "180", "195", "175", "190", 10),
          makeBar(HistStart + 60000, "190", "205", "185", "200", 20),
      }},
      makeCalendar());
  auto Tl = std::make_shared<core::Timeline>(
      core::Timeline::build(
          Instruments,
          {{
              makeBar(SessStart, "200", "220", "190", "210", 30),
          }},
          makeCalendar())
          .withWarmup(Hist, 2));

  const auto Scales =
      core::computeBlindScales(*Tl, core::Decimal::fromUnits(100000), 0);
  ASSERT_EQ(Scales.size(), 1u);
  ASSERT_TRUE(Scales[0].P0.has_value());
  EXPECT_EQ(*Scales[0].P0, core::Decimal::fromUnits(200));

  const auto Warmup = Tl->warmupBars();
  ASSERT_EQ(Warmup.size(), 2u);
  const core::BlindBar W0 = core::toBlindBar(Warmup[0], Scales[0]);
  EXPECT_EQ(W0.Step, -2);
  EXPECT_EQ(W0.Open, core::Decimal::parse("90.00"));
  EXPECT_EQ(W0.Close, core::Decimal::parse("95.00"));
  // V_ref = (10 + 20) / 2 = 15; 100 * 10 / 15 = 66.67 (HALF_UP)
  EXPECT_EQ(W0.VolumeRel, core::Decimal::parse("66.67"));

  const core::BlindBar W1 = core::toBlindBar(Warmup[1], Scales[0]);
  EXPECT_EQ(W1.Step, -1);
  EXPECT_EQ(W1.Open, core::Decimal::parse("95.00"));
  EXPECT_EQ(W1.Close, core::Decimal::parse("100.00"));
  EXPECT_EQ(W1.VolumeRel, core::Decimal::parse("133.33"));

  const core::BlindBar S0 = core::toBlindBar(*Tl->barOf(0, 0), Scales[0]);
  EXPECT_EQ(S0.VolumeRel, core::Decimal::parse("200.00"));
}

TEST(BlindScale, ZeroVolumeFallsBackToOneLot) {
  std::vector<core::Instrument> Instruments = {
      {"A", "TKA", 10, core::Decimal::parse("0.01")},
  };
  const std::int64_t SessStart = 1736751600000LL;
  auto Tl = std::make_shared<core::Timeline>(core::Timeline::build(
      Instruments,
      {{
          makeBar(SessStart, "100", "105", "95", "102", 0),
          makeBar(SessStart + 60000, "102", "108", "100", "105", 50),
      }},
      makeCalendar()));

  const auto Scales =
      core::computeBlindScales(*Tl, core::Decimal::fromUnits(100000), 1);
  EXPECT_EQ(core::toBlindBar(*Tl->barOf(0, 0), Scales[0]).VolumeRel,
            core::Decimal::parse("0.00"));
  EXPECT_EQ(core::toBlindBar(*Tl->barOf(0, 1), Scales[0]).VolumeRel,
            core::Decimal::parse("500.00"));
}

TEST(BlindScale, PercentOfCapitalHalfUp) {
  const core::Decimal Cash = core::Decimal::fromUnits(100000);
  EXPECT_EQ(core::toPercentOfCapital(core::Decimal::parse("1234"), Cash),
            core::Decimal::parse("1.23"));
  EXPECT_EQ(core::toPercentOfCapital(core::Decimal::parse("1235"), Cash),
            core::Decimal::parse("1.24"));
  EXPECT_EQ(core::toPercentOfCapital(core::Decimal::parse("-1235"), Cash),
            core::Decimal::parse("-1.24"));
}

TEST(BlindScale, LotValueAtStartNullBeforeFirstCandle) {
  std::vector<core::Instrument> Instruments = {
      {"A", "TKA", 10, core::Decimal::parse("0.01")},
      {"B", "TKB", 5, core::Decimal::parse("0.01")},
  };
  const std::int64_t SessStart = 1736751600000LL;
  auto Tl = std::make_shared<core::Timeline>(core::Timeline::build(
      Instruments,
      {
          {
              makeBar(SessStart, "200", "210", "195", "205", 10),
              makeBar(SessStart + 60000, "205", "215", "200", "210", 12),
          },
          {
              makeBar(SessStart + 60000, "500", "510", "490", "505", 8),
          },
      },
      makeCalendar()));

  const core::Decimal Cash = core::Decimal::fromUnits(100000);
  const auto ScalesAt0 = core::computeBlindScales(*Tl, Cash, 0);
  EXPECT_TRUE(ScalesAt0[0].LotValueAtStart.has_value());
  EXPECT_EQ(*ScalesAt0[0].LotValueAtStart, core::Decimal::parse("2.00"));
  EXPECT_FALSE(ScalesAt0[1].LotValueAtStart.has_value());
  EXPECT_FALSE(ScalesAt0[1].P0.has_value());

  const auto ScalesAt1 = core::computeBlindScales(*Tl, Cash, 1);
  ASSERT_TRUE(ScalesAt1[1].LotValueAtStart.has_value());
  // 500 * 5 * 100 / 100000 = 2.50
  EXPECT_EQ(*ScalesAt1[1].LotValueAtStart, core::Decimal::parse("2.50"));
  ASSERT_TRUE(ScalesAt1[1].P0.has_value());
  EXPECT_EQ(*ScalesAt1[1].P0, core::Decimal::fromUnits(500));
}

TEST(BlindOrderPrices, ResolvesBuyCeilingAndSellFloorToPriceStep) {
  std::vector<core::Instrument> Instruments = {
      {"A", "TKA", 10, core::Decimal::parse("0.05")},
  };
  const std::int64_t SessStart = 1736751600000LL;
  auto Tl = std::make_shared<core::Timeline>(core::Timeline::build(
      Instruments,
      {{
          makeBar(SessStart, "200", "220", "190", "210", 10),
          makeBar(SessStart + 60000, "210", "230", "200", "220", 10),
      }},
      makeCalendar()));

  EXPECT_EQ(core::blindP0(*Tl, 0, 0), core::Decimal::fromUnits(200));

  core::OrderSpec BuyLimit;
  BuyLimit.Instrument = 0;
  BuyLimit.OrderSide = core::Side::Buy;
  BuyLimit.Type = core::OrderType::Limit;
  BuyLimit.Lots = 1;
  BuyLimit.LimitPrice =
      core::Decimal::parse("100.11"); // 200.22 -> CEILING 200.25
  BuyLimit.BlindPrices = true;

  const core::OrderSpec ResolvedBuy =
      core::resolveBlindOrderPrices(BuyLimit, *Tl, 0);
  EXPECT_FALSE(ResolvedBuy.BlindPrices);
  ASSERT_TRUE(ResolvedBuy.LimitPrice.has_value());
  EXPECT_EQ(*ResolvedBuy.LimitPrice, core::Decimal::parse("200.25"));

  core::OrderSpec SellStopLimit;
  SellStopLimit.Instrument = 0;
  SellStopLimit.OrderSide = core::Side::Sell;
  SellStopLimit.Type = core::OrderType::StopLimit;
  SellStopLimit.Lots = 1;
  SellStopLimit.StopPrice =
      core::Decimal::parse("99.89"); // 199.78 -> FLOOR 199.75
  SellStopLimit.LimitPrice =
      core::Decimal::parse("99.88"); // 199.76 -> FLOOR 199.75
  SellStopLimit.BlindPrices = true;

  const core::OrderSpec ResolvedSell =
      core::resolveBlindOrderPrices(SellStopLimit, *Tl, 0);
  EXPECT_FALSE(ResolvedSell.BlindPrices);
  ASSERT_TRUE(ResolvedSell.StopPrice.has_value());
  ASSERT_TRUE(ResolvedSell.LimitPrice.has_value());
  EXPECT_EQ(*ResolvedSell.StopPrice, core::Decimal::parse("199.75"));
  EXPECT_EQ(*ResolvedSell.LimitPrice, core::Decimal::parse("199.75"));

  // Обычная рублёвая заявка не меняется.
  core::OrderSpec RubleOrder = BuyLimit;
  RubleOrder.BlindPrices = false;
  RubleOrder.LimitPrice = core::Decimal::parse("200.22");
  const core::OrderSpec Unchanged =
      core::resolveBlindOrderPrices(RubleOrder, *Tl, 0);
  EXPECT_FALSE(Unchanged.BlindPrices);
  EXPECT_EQ(*Unchanged.LimitPrice, core::Decimal::parse("200.22"));
}

TEST(BlindOrderPrices, RejectsWithoutP0AndRespectsInv1) {
  std::vector<core::Instrument> Instruments = {
      {"A", "TKA", 10, core::Decimal::parse("0.01")},
      {"B", "TKB", 5, core::Decimal::parse("0.01")},
  };
  const std::int64_t SessStart = 1736751600000LL;
  auto Tl = std::make_shared<core::Timeline>(core::Timeline::build(
      Instruments,
      {
          {
              makeBar(SessStart, "200", "210", "195", "205", 10),
              makeBar(SessStart + 60000, "205", "215", "200", "210", 12),
          },
          {
              // У B нет бара на шаге 0; первый бар на шаге 1.
              makeBar(SessStart + 60000, "500", "510", "490", "505", 8),
          },
      },
      makeCalendar()));

  EXPECT_FALSE(core::blindP0(*Tl, 1, 0).has_value());
  EXPECT_EQ(core::blindP0(*Tl, 1, 1), core::Decimal::fromUnits(500));

  core::OrderSpec BlindOnB;
  BlindOnB.Instrument = 1;
  BlindOnB.OrderSide = core::Side::Buy;
  BlindOnB.Type = core::OrderType::Limit;
  BlindOnB.Lots = 1;
  BlindOnB.LimitPrice = core::Decimal::parse("100.00");
  BlindOnB.BlindPrices = true;

  EXPECT_THROW(core::resolveBlindOrderPrices(BlindOnB, *Tl, 0),
               std::invalid_argument);
  const core::OrderSpec AtStep1 =
      core::resolveBlindOrderPrices(BlindOnB, *Tl, 1);
  EXPECT_EQ(*AtStep1.LimitPrice, core::Decimal::parse("500.00"));
}

TEST(BlindOrderPrices, ThrowsDecimalOverflowOnHugeBlindPrice) {
  std::vector<core::Instrument> Instruments = {
      {"A", "TKA", 10, core::Decimal::parse("0.01")},
  };
  const std::int64_t SessStart = 1736751600000LL;
  auto Tl = std::make_shared<core::Timeline>(core::Timeline::build(
      Instruments,
      {{makeBar(SessStart, "1000000000000000000", "1000000000000000000",
                "1000000000000000000", "1000000000000000000", 10)}},
      makeCalendar()));

  core::OrderSpec Blind;
  Blind.Instrument = 0;
  Blind.OrderSide = core::Side::Buy;
  Blind.Type = core::OrderType::Limit;
  Blind.Lots = 1;
  Blind.LimitPrice = core::Decimal::parse("1000000000000000000");
  Blind.BlindPrices = true;

  EXPECT_THROW(core::resolveBlindOrderPrices(Blind, *Tl, 0),
               core::DecimalOverflow);
}
