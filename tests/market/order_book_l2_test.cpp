#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/market/order_book_l2.hpp"

namespace {

using core::Bar;
using core::BookFillResult;
using core::buildOrderBook;
using core::Decimal;
using core::OrderBookConfig;
using core::OrderBookLevel;
using core::OrderBookSnapshot;
using core::RoundingMode;
using core::Side;
using core::synthesizeTape;
using core::TapePrint;
using core::walkBookFill;

Bar makeSampleBar(const char *Open, const char *High, const char *Low,
                  const char *Close, std::int64_t Volume = 10000) {
  Bar Sample;
  Sample.Index = 0;
  Sample.TsOpenMs = 1700000000000;
  Sample.AvailableAtMs = 1700000060000;
  Sample.Open = Decimal::parse(Open);
  Sample.High = Decimal::parse(High);
  Sample.Low = Decimal::parse(Low);
  Sample.Close = Decimal::parse(Close);
  Sample.Volume = Volume;
  Sample.Value = Sample.Close.times(Volume);
  return Sample;
}

TEST(OrderBookL2, BuildsOrderedQuantizedLevelsWithPositiveLots) {
  const Bar Sample = makeSampleBar("250.00", "252.50", "249.10", "251.20", 5000);
  OrderBookConfig Cfg;
  Cfg.Depth = 10;
  Cfg.SpreadPct = Decimal::parse("0.001"); // 10 bps
  Cfg.PriceStep = Decimal::parse("0.05");

  const OrderBookSnapshot Book = buildOrderBook(Sample, Cfg);

  ASSERT_EQ(Book.Bids.size(), 10u);
  ASSERT_EQ(Book.Asks.size(), 10u);
  EXPECT_EQ(Book.TimeUnixMs, Sample.AvailableAtMs);
  EXPECT_LT(Book.Bids.front().Price, Book.Asks.front().Price);
  EXPECT_GT(Book.MidPrice, Book.Bids.front().Price);
  EXPECT_LT(Book.MidPrice, Book.Asks.front().Price);
  EXPECT_GT(Book.SpreadBps, Decimal::zero());

  for (std::size_t I = 0; I < Book.Bids.size(); ++I) {
    EXPECT_GT(Book.Bids[I].Price, Decimal::zero());
    EXPECT_GT(Book.Bids[I].Lots, 0);
    EXPECT_EQ(Book.Bids[I].Price,
              Book.Bids[I].Price.roundToStep(Cfg.PriceStep,
                                             RoundingMode::HalfUp));
    if (I > 0) {
      EXPECT_LT(Book.Bids[I].Price, Book.Bids[I - 1].Price);
      EXPECT_EQ(Book.Bids[I - 1].Price - Book.Bids[I].Price, Cfg.PriceStep);
    }
  }

  for (std::size_t I = 0; I < Book.Asks.size(); ++I) {
    EXPECT_GT(Book.Asks[I].Price, Decimal::zero());
    EXPECT_GT(Book.Asks[I].Lots, 0);
    EXPECT_EQ(Book.Asks[I].Price,
              Book.Asks[I].Price.roundToStep(Cfg.PriceStep,
                                             RoundingMode::HalfUp));
    if (I > 0) {
      EXPECT_GT(Book.Asks[I].Price, Book.Asks[I - 1].Price);
      EXPECT_EQ(Book.Asks[I].Price - Book.Asks[I - 1].Price, Cfg.PriceStep);
    }
  }
}

TEST(OrderBookL2, VolumeScalesProportionallyAndIsDeterministic) {
  const Bar SmallBar =
      makeSampleBar("100.00", "101.00", "99.00", "100.50", 1000);
  const Bar LargeBar =
      makeSampleBar("100.00", "101.00", "99.00", "100.50", 10000);
  OrderBookConfig Cfg;
  Cfg.Depth = 5;
  Cfg.PriceStep = Decimal::parse("0.01");

  const OrderBookSnapshot SmallA = buildOrderBook(SmallBar, Cfg);
  const OrderBookSnapshot SmallB = buildOrderBook(SmallBar, Cfg);
  EXPECT_EQ(SmallA, SmallB);

  const OrderBookSnapshot Large = buildOrderBook(LargeBar, Cfg);
  std::int64_t SmallBidTotal = 0;
  std::int64_t LargeBidTotal = 0;
  for (std::size_t I = 0; I < SmallA.Bids.size(); ++I) {
    SmallBidTotal += SmallA.Bids[I].Lots;
    LargeBidTotal += Large.Bids[I].Lots;
  }
  EXPECT_GT(LargeBidTotal, SmallBidTotal * 5);
}

TEST(OrderBookL2, SynthesizesOhlcConsistentDeterministicTape) {
  const Bar BullBar =
      makeSampleBar("100.00", "103.00", "99.00", "102.00", 1234);
  OrderBookConfig Cfg;
  Cfg.PriceStep = Decimal::parse("0.01");
  Cfg.TapePrintsPerBar = 6;

  const std::vector<TapePrint> TapeA = synthesizeTape(BullBar, Cfg);
  const std::vector<TapePrint> TapeB = synthesizeTape(BullBar, Cfg);
  EXPECT_EQ(TapeA, TapeB);

  ASSERT_EQ(TapeA.size(), 6u);
  std::int64_t TotalLots = 0;
  bool SawLow = false;
  bool SawHigh = false;
  for (std::size_t I = 0; I < TapeA.size(); ++I) {
    EXPECT_GE(TapeA[I].Price, BullBar.Low);
    EXPECT_LE(TapeA[I].Price, BullBar.High);
    EXPECT_GT(TapeA[I].Lots, 0);
    TotalLots += TapeA[I].Lots;
    if (I > 0) {
      EXPECT_GE(TapeA[I].TimeUnixMs, TapeA[I - 1].TimeUnixMs);
    }
    if (TapeA[I].Price == BullBar.Low) {
      SawLow = true;
    }
    if (TapeA[I].Price == BullBar.High) {
      SawHigh = true;
    }
  }
  EXPECT_EQ(TotalLots, BullBar.Volume);
  EXPECT_EQ(TapeA.front().Price, BullBar.Open);
  EXPECT_EQ(TapeA.back().Price, BullBar.Close);
  EXPECT_TRUE(SawLow);
  EXPECT_TRUE(SawHigh);
}

TEST(OrderBookL2, SynthesizeTapeHandlesBearishSmallAndZeroVolume) {
  OrderBookConfig Cfg;
  Cfg.PriceStep = Decimal::parse("0.01");
  Cfg.TapePrintsPerBar = 6;

  const Bar BearBar = makeSampleBar("102.00", "103.00", "98.00", "99.00", 500);
  const auto BearTape = synthesizeTape(BearBar, Cfg);
  ASSERT_EQ(BearTape.size(), 6u);
  EXPECT_EQ(BearTape.front().AggressorSide, Side::Sell);
  std::int64_t BearTotal = 0;
  for (const auto &Print : BearTape) {
    BearTotal += Print.Lots;
    EXPECT_GE(Print.Price, BearBar.Low);
    EXPECT_LE(Print.Price, BearBar.High);
  }
  EXPECT_EQ(BearTotal, 500);

  const Bar TinyBar = makeSampleBar("100.00", "101.00", "99.00", "100.50", 3);
  const auto TinyTape = synthesizeTape(TinyBar, Cfg);
  ASSERT_EQ(TinyTape.size(), 3u);
  EXPECT_EQ(TinyTape[0].Lots + TinyTape[1].Lots + TinyTape[2].Lots, 3);

  const Bar ZeroBar = makeSampleBar("100.00", "100.00", "100.00", "100.00", 0);
  EXPECT_TRUE(synthesizeTape(ZeroBar, Cfg).empty());
}

TEST(OrderBookL2, WalkBookFillBuyExactVwapAndPartialFill) {
  OrderBookSnapshot Book;
  Book.Asks = {
      OrderBookLevel{Decimal::parse("100.00"), 10},
      OrderBookLevel{Decimal::parse("101.00"), 20},
      OrderBookLevel{Decimal::parse("102.00"), 30},
  };

  // Полное исполнение на одном уровне: нулевое рыночное воздействие.
  const BookFillResult TopFill = walkBookFill(Book, Side::Buy, 10);
  EXPECT_EQ(TopFill.FilledLots, 10);
  EXPECT_EQ(TopFill.RemainingLots, 0);
  EXPECT_EQ(TopFill.AvgPrice, Decimal::parse("100.00"));
  EXPECT_EQ(TopFill.TotalValue, Decimal::parse("1000.00"));
  EXPECT_EQ(TopFill.PriceImpactBps, Decimal::zero());

  // Проедание двух уровней: 10 * 100 + 10 * 101 = 2010 -> VWAP = 100.50,
  // impact = 50 bps.
  const BookFillResult MultiFill = walkBookFill(Book, Side::Buy, 20);
  EXPECT_EQ(MultiFill.FilledLots, 20);
  EXPECT_EQ(MultiFill.RemainingLots, 0);
  EXPECT_EQ(MultiFill.AvgPrice, Decimal::parse("100.50"));
  EXPECT_EQ(MultiFill.TotalValue, Decimal::parse("2010.00"));
  EXPECT_EQ(MultiFill.PriceImpactBps, Decimal::fromUnits(50));

  // Частичное исполнение: запрошено 100, в стакане всего 60 лотов.
  const BookFillResult PartialFill = walkBookFill(Book, Side::Buy, 100);
  EXPECT_EQ(PartialFill.FilledLots, 60);
  EXPECT_EQ(PartialFill.RemainingLots, 40);
  EXPECT_EQ(PartialFill.TotalValue, Decimal::parse("6080.00"));
  EXPECT_EQ(PartialFill.AvgPrice,
            Decimal::parse("6080").div(Decimal::fromUnits(60),
                                       RoundingMode::HalfUp));
  EXPECT_GT(PartialFill.PriceImpactBps, Decimal::fromUnits(133));
}

TEST(OrderBookL2, WalkBookFillSellExactVwapAndEdgeCases) {
  OrderBookSnapshot Book;
  Book.Bids = {
      OrderBookLevel{Decimal::parse("100.00"), 10},
      OrderBookLevel{Decimal::parse("99.00"), 10},
  };

  // Продажа 20 лотов: 10 * 100 + 10 * 99 = 1990 -> VWAP = 99.50, impact = 50
  // bps.
  const BookFillResult SellFill = walkBookFill(Book, Side::Sell, 20);
  EXPECT_EQ(SellFill.FilledLots, 20);
  EXPECT_EQ(SellFill.RemainingLots, 0);
  EXPECT_EQ(SellFill.AvgPrice, Decimal::parse("99.50"));
  EXPECT_EQ(SellFill.TotalValue, Decimal::parse("1990.00"));
  EXPECT_EQ(SellFill.PriceImpactBps, Decimal::fromUnits(50));

  // Пустая сторона стакана.
  const BookFillResult EmptySide = walkBookFill(Book, Side::Buy, 5);
  EXPECT_EQ(EmptySide.FilledLots, 0);
  EXPECT_EQ(EmptySide.RemainingLots, 5);
  EXPECT_EQ(EmptySide.AvgPrice, Decimal::zero());
  EXPECT_EQ(EmptySide.PriceImpactBps, Decimal::zero());

  // Запрос 0 лотов.
  const BookFillResult ZeroReq = walkBookFill(Book, Side::Sell, 0);
  EXPECT_EQ(ZeroReq.FilledLots, 0);
  EXPECT_EQ(ZeroReq.RemainingLots, 0);
}

TEST(OrderBookL2, RejectsInvalidArguments) {
  const Bar Sample = makeSampleBar("100.00", "101.00", "99.00", "100.00", 100);
  OrderBookConfig BadDepth;
  BadDepth.Depth = 0;
  EXPECT_THROW(buildOrderBook(Sample, BadDepth), std::invalid_argument);

  OrderBookConfig BadStep;
  BadStep.PriceStep = Decimal::parse("-0.01");
  EXPECT_THROW(buildOrderBook(Sample, BadStep), std::invalid_argument);

  OrderBookConfig BadPrints;
  BadPrints.TapePrintsPerBar = 0;
  EXPECT_THROW(synthesizeTape(Sample, BadPrints), std::invalid_argument);

  OrderBookSnapshot EmptyBook;
  EXPECT_THROW(walkBookFill(EmptyBook, Side::Buy, -1), std::invalid_argument);
}

} // namespace
