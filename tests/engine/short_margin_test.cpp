#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "core/engine/digest.hpp"
#include "core/engine/session_engine.hpp"
#include "core/market/date_utils.hpp"
#include "core/market/timeline.hpp"
#include "core/report/metrics.hpp"
#include "core/report/metrics_input.hpp"

namespace {

using core::Bar;
using core::CostConfig;
using core::Decimal;
using core::Fill;
using core::Instrument;
using core::JournalEntry;
using core::MarginConfig;
using core::OrderSpec;
using core::OrderStatus;
using core::OrderType;
using core::PlaceCmd;
using core::Reason;
using core::ReplayResult;
using core::Report;
using core::SessionEngine;
using core::SessionSpec;
using core::SessionStatus;
using core::Side;
using core::TimeInForce;
using core::Timeline;
using core::TradingDay;

Decimal d(const char *Text) { return Decimal::parse(Text); }

Instrument makeInst(const char *Key = "A", std::int64_t LotSize = 10) {
  return Instrument{Key, Key, LotSize, d("0.01")};
}

Bar makeBar(std::int32_t Ymd, const char *Open, const char *Close,
            std::int64_t Volume = 100000) {
  Bar B;
  B.TsOpenMs = core::moscowDayStartMs(Ymd) + 10 * core::kHourMs;
  B.AvailableAtMs = B.TsOpenMs + core::kDayMs;
  B.Open = d(Open);
  B.High = std::max(B.Open, d(Close));
  B.Low = std::min(B.Open, d(Close));
  B.Close = d(Close);
  B.Volume = Volume;
  return B;
}

std::shared_ptr<const Timeline>
makeTimeline(const std::vector<Bar> &Bars,
             const std::vector<std::int32_t> &Dates,
             std::int64_t LotSize = 10) {
  std::vector<TradingDay> Cal;
  Cal.reserve(Dates.size());
  for (std::int32_t Ymd : Dates)
    Cal.push_back(TradingDay{Ymd, true});
  return std::make_shared<const Timeline>(
      Timeline::build({makeInst("A", LotSize)}, {Bars}, Cal));
}

OrderSpec marketOrder(Side OrderSide, std::int64_t Lots, int Instr = 0) {
  OrderSpec S;
  S.Instrument = Instr;
  S.OrderSide = OrderSide;
  S.Type = OrderType::Market;
  S.Lots = Lots;
  return S;
}

JournalEntry placeAt(std::int64_t Seq, std::int64_t Step, OrderSpec Spec) {
  JournalEntry E;
  E.Seq = Seq;
  E.Step = Step;
  E.Command = PlaceCmd{std::move(Spec), std::nullopt};
  return E;
}

TEST(ShortMargin, DefaultConfigUnchanged) {
  const std::vector<Bar> Bars = {
      makeBar(20240110, "100", "100"),
      makeBar(20240111, "100", "100"),
  };
  const std::vector<std::int32_t> Dates = {20240110, 20240111};
  const auto Tl = makeTimeline(Bars, Dates);

  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs = CostConfig::zero();
  Spec.InitialCash = d("10000");

  EXPECT_FALSE(Spec.Margin.AllowShort);
  EXPECT_EQ(Spec.Margin.MaxLeverage, Decimal::one());
  EXPECT_FALSE(Spec.Margin.isMarginEnabled());

  const std::vector<JournalEntry> Journal = {
      placeAt(0, 0, marketOrder(Side::Sell, 1)),
      placeAt(1, 0, marketOrder(Side::Buy, 15)),
  };
  const ReplayResult Res = core::replaySession(Spec, Tl, Journal, 1, 0);
  ASSERT_EQ(Res.CommandResults.size(), 2u);
  EXPECT_FALSE(Res.CommandResults[0].Accepted);
  EXPECT_EQ(Res.CommandResults[0].ReasonCode, Reason::InsufficientPosition);
  EXPECT_FALSE(Res.CommandResults[1].Accepted);
  EXPECT_EQ(Res.CommandResults[1].ReasonCode, Reason::InsufficientCash);
  EXPECT_TRUE(Res.Engine.fills().empty());
}

TEST(ShortMargin, MarginConfigValidation) {
  MarginConfig BadLev;
  BadLev.MaxLeverage = d("0.5");
  EXPECT_THROW(core::validateMarginConfig(BadLev), std::invalid_argument);

  MarginConfig BadBorrow;
  BadBorrow.ShortBorrowAnnualPct = d("-1");
  EXPECT_THROW(core::validateMarginConfig(BadBorrow), std::invalid_argument);

  MarginConfig BadLoan;
  BadLoan.MarginLoanAnnualPct = d("-0.1");
  EXPECT_THROW(core::validateMarginConfig(BadLoan), std::invalid_argument);

  MarginConfig BadInit;
  BadInit.InitialMarginRatio = d("1.5");
  EXPECT_THROW(core::validateMarginConfig(BadInit), std::invalid_argument);

  MarginConfig BadMaint;
  BadMaint.MaxLeverage = d("2");
  BadMaint.MaintenanceMarginRatio = d("0.6");
  EXPECT_THROW(core::validateMarginConfig(BadMaint), std::invalid_argument);
}

TEST(ShortMargin, ShortOpenProfitAndLossFifo) {
  std::vector<Bar> Bars;
  Bars.push_back(makeBar(20240110, "100", "100"));
  Bars.push_back(makeBar(20240111, "100", "90"));
  Bars.push_back(makeBar(20240112, "80", "80"));
  Bars.push_back(makeBar(20240115, "80", "100"));
  Bars.push_back(makeBar(20240116, "120", "120"));
  const std::vector<std::int32_t> Dates = {
      20240110, 20240111, 20240112, 20240115, 20240116,
  };
  const auto Tl = makeTimeline(Bars, Dates);

  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs = CostConfig::zero();
  Spec.InitialCash = d("100000");
  Spec.Margin.AllowShort = true;

  // Шаг 0: шорт 10 лотов (исполнится на шаге 1 по open 100 -> -10 лотов,
  // Cash = 110000, на close 90 Equity = 101000).
  // Шаг 1: выкуп 10 лотов (исполнится на шаге 2 по open 80 -> +2000 прибыли).
  // Шаг 2: шорт 10 лотов (исполнится на шаге 3 по open 80).
  // Шаг 3: выкуп 10 лотов (исполнится на шаге 4 по open 120 -> -4000 убытка).
  const std::vector<JournalEntry> Journal = {
      placeAt(0, 0, marketOrder(Side::Sell, 10)),
      placeAt(1, 1, marketOrder(Side::Buy, 10)),
      placeAt(2, 2, marketOrder(Side::Sell, 10)),
      placeAt(3, 3, marketOrder(Side::Buy, 10)),
  };
  const ReplayResult Res = core::replaySession(Spec, Tl, Journal, 4, 0);
  ASSERT_EQ(Res.Engine.fills().size(), 4u);
  EXPECT_EQ(Res.Engine.equity()[1].Equity, d("101000.00"));
  EXPECT_EQ(Res.Engine.equity()[2].Equity, d("102000.00"));
  EXPECT_EQ(Res.Engine.equity()[4].Equity, d("98000.00"));
  EXPECT_EQ(Res.Engine.portfolio().lots(0), 0);

  const auto &Trips = Res.Engine.portfolio().roundTrips();
  ASSERT_EQ(Trips.size(), 2u);
  EXPECT_TRUE(Trips[0].IsShort);
  EXPECT_EQ(Trips[0].EntryPrice, d("100"));
  EXPECT_EQ(Trips[0].ExitPrice, d("80"));
  EXPECT_EQ(Trips[0].grossPnl(), d("2000.00"));
  EXPECT_EQ(Trips[0].NetPnl, d("2000.00"));

  EXPECT_TRUE(Trips[1].IsShort);
  EXPECT_EQ(Trips[1].EntryPrice, d("80"));
  EXPECT_EQ(Trips[1].ExitPrice, d("120"));
  EXPECT_EQ(Trips[1].grossPnl(), d("-4000.00"));
  EXPECT_EQ(Trips[1].NetPnl, d("-4000.00"));
}

TEST(ShortMargin, PositionFlipAndPartialCover) {
  const std::vector<Bar> Bars = {
      makeBar(20240110, "100", "100"),
      makeBar(20240111, "100", "100"),
      makeBar(20240112, "110", "110"),
      makeBar(20240115, "90", "90"),
  };
  const std::vector<std::int32_t> Dates = {20240110, 20240111, 20240112,
                                           20240115};
  const auto Tl = makeTimeline(Bars, Dates);

  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs = CostConfig::zero();
  Spec.InitialCash = d("100000");
  Spec.Margin.AllowShort = true;

  // Переворот: покупка 5, затем продажа 15 (закрыть 5 + открыть 10 шорт),
  // затем частичное покрытие 4 из 10 шорта.
  const std::vector<JournalEntry> Journal = {
      placeAt(0, 0, marketOrder(Side::Buy, 5)),
      placeAt(1, 1, marketOrder(Side::Sell, 15)),
      placeAt(2, 2, marketOrder(Side::Buy, 4)),
  };
  const ReplayResult Res = core::replaySession(Spec, Tl, Journal, 3, 0);
  EXPECT_EQ(Res.Engine.portfolio().lots(0), -6);
  EXPECT_EQ(Res.Engine.portfolio().avgPrice(0), d("110"));
  EXPECT_EQ(Res.Engine.portfolio().costBasis(0), d("-6600.00"));

  const auto &Trips = Res.Engine.portfolio().roundTrips();
  ASSERT_EQ(Trips.size(), 2u);
  EXPECT_FALSE(Trips[0].IsShort);
  EXPECT_EQ(Trips[0].Lots, 5);
  EXPECT_EQ(Trips[0].NetPnl, d("500.00"));
  EXPECT_TRUE(Trips[1].IsShort);
  EXPECT_EQ(Trips[1].Lots, 4);
  EXPECT_EQ(Trips[1].NetPnl, d("800.00"));
}

TEST(ShortMargin, OvernightShortBorrowFeeAcrossWeekend) {
  // 2024-01-12 (пятница) -> 2024-01-15 (понедельник) = 3 календарных дня.
  const std::vector<Bar> Bars = {
      makeBar(20240111, "100", "100"),
      makeBar(20240112, "100", "100"),
      makeBar(20240115, "100", "100"),
  };
  const std::vector<std::int32_t> Dates = {20240111, 20240112, 20240115};
  const auto Tl = makeTimeline(Bars, Dates);

  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs = CostConfig::zero();
  Spec.InitialCash = d("100000");
  Spec.Margin.AllowShort = true;
  // 36.5% годовых -> 0.1% в календарный день; за 3 дня = 0.3%.
  Spec.Margin.ShortBorrowAnnualPct = d("36.5");

  const std::vector<JournalEntry> Journal = {
      placeAt(0, 0, marketOrder(Side::Sell, 10)),
  };
  const ReplayResult Res = core::replaySession(Spec, Tl, Journal, 2, 0);
  // 10000 * 36.5 * 3 / 36500 = 30.00 ₽
  EXPECT_EQ(Res.Engine.overnightFees(), d("30.00"));
  EXPECT_EQ(Res.Engine.equity()[2].Equity, d("99970.00"));

  core::MetricsInput In =
      core::buildMetricsInput(Res.Engine, Spec.InitialCash, *Tl);
  In.RuoniaRates = {d("16"), d("16"), d("16")};
  In.CalendarDays = {1, 1, 3};
  const Report Rep = core::computeReport(In);
  EXPECT_EQ(Rep.Costs.OvernightFee, d("30.00"));
  EXPECT_EQ(Rep.Costs.Total, d("30.00"));
}

TEST(ShortMargin, LeverageBuyingPowerAndMarginLoanInterest) {
  const std::vector<Bar> Bars = {
      makeBar(20240110, "100", "100"),
      makeBar(20240111, "100", "100"),
      makeBar(20240112, "100", "100"),
  };
  const std::vector<std::int32_t> Dates = {20240110, 20240111, 20240112};
  const auto Tl = makeTimeline(Bars, Dates);

  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs = CostConfig::zero();
  Spec.InitialCash = d("100000");
  // Dнач = 0.5 -> покупательная способность 200000; 0.1% в календарный день.
  Spec.Margin.MaxLeverage = d("2");
  Spec.Margin.MarginLoanAnnualPct = d("36.5");

  // 250 лотов = 250000 > 200000 -> отказ InsufficientMargin.
  // 180 лотов = 180000 <= 200000 -> принято; Cash = -80000.
  const std::vector<JournalEntry> Journal = {
      placeAt(0, 0, marketOrder(Side::Buy, 250)),
      placeAt(1, 0, marketOrder(Side::Buy, 180)),
  };
  const ReplayResult Res = core::replaySession(Spec, Tl, Journal, 2, 0);
  ASSERT_EQ(Res.CommandResults.size(), 2u);
  EXPECT_FALSE(Res.CommandResults[0].Accepted);
  EXPECT_EQ(Res.CommandResults[0].ReasonCode, Reason::InsufficientMargin);
  EXPECT_TRUE(Res.CommandResults[1].Accepted);
  EXPECT_EQ(Res.Engine.portfolio().lots(0), 180);
  // За 1 календарный день (11 -> 12 янв) на заём 80000: 80000 * 0.001 = 80.00
  EXPECT_EQ(Res.Engine.overnightFees(), d("80.00"));
  EXPECT_EQ(Res.Engine.portfolio().cash(), d("-80080.00"));
  EXPECT_EQ(Res.Engine.equity()[2].Equity, d("99920.00"));
}

TEST(ShortMargin, MaintenanceMarginLiquidationAndDeterminism) {
  const std::vector<Bar> Bars = {
      makeBar(20240110, "100", "100"),
      makeBar(20240111, "100", "100"),
      makeBar(20240112, "60", "60"),
      makeBar(20240115, "55", "55"),
  };
  const std::vector<std::int32_t> Dates = {20240110, 20240111, 20240112,
                                           20240115};
  const auto Tl = makeTimeline(Bars, Dates);

  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs = CostConfig::zero();
  Spec.InitialCash = d("100000");
  Spec.Margin.MaxLeverage = d("2");
  Spec.Margin.InitialMarginRatio = d("0.5");
  Spec.Margin.MaintenanceMarginRatio = d("0.25");

  OrderSpec Lim;
  Lim.Instrument = 0;
  Lim.OrderSide = Side::Buy;
  Lim.Type = OrderType::Limit;
  Lim.Lots = 1;
  Lim.LimitPrice = d("10");
  Lim.Tif = TimeInForce::Gtc;

  // Шаг 0: покупка 180 лотов (исполнится на шаге 1 по 100 -> Cash = -80000).
  // Шаг 1: активная GTC лимитка (должна сняться при принудительном закрытии).
  // Шаг 2: close = 60 -> Gross = 108000, Equity = 28000 >= Dmin (27000).
  // Шаг 3: close = 55 -> Gross = 99000, Equity = 19000 < Dmin (24750).
  const std::vector<JournalEntry> Journal = {
      placeAt(0, 0, marketOrder(Side::Buy, 180)),
      placeAt(1, 1, Lim),
  };

  const ReplayResult Res1 = core::replaySession(Spec, Tl, Journal, 3, 0);
  const ReplayResult Res2 = core::replaySession(Spec, Tl, Journal, 3, 0);

  EXPECT_EQ(Res1.Engine.marginCallCount(), 1);
  EXPECT_EQ(Res1.Engine.liquidationCount(), 1);
  EXPECT_EQ(Res1.Engine.portfolio().lots(0), 0);
  EXPECT_EQ(Res1.Engine.portfolio().cash(), d("19000.00"));
  EXPECT_EQ(Res1.Engine.equity()[3].Equity, d("19000.00"));
  EXPECT_EQ(Res1.Status, SessionStatus::FinishedEnd);

  ASSERT_EQ(Res1.Engine.fills().size(), 2u);
  const Fill &LiqFill = Res1.Engine.fills()[1];
  EXPECT_TRUE(LiqFill.IsLiquidation);
  EXPECT_EQ(LiqFill.OrderId, -1);
  EXPECT_EQ(LiqFill.OrderSide, Side::Sell);
  EXPECT_EQ(LiqFill.Lots, 180);
  EXPECT_EQ(LiqFill.Price, d("55"));

  ASSERT_EQ(Res1.Engine.orders().size(), 2u);
  EXPECT_EQ(Res1.Engine.orders()[1].Status, OrderStatus::Cancelled);
  EXPECT_EQ(Res1.Engine.orders()[1].ReasonCode, Reason::MarginCallLiquidation);

  // INV-4: детерминизм (одинаковый дайджест при повторном прогоне).
  EXPECT_EQ(core::digestOf(Res1.Engine), core::digestOf(Res2.Engine));
}

TEST(ShortMargin, LiquidationIntoBankruptcy) {
  const std::vector<Bar> Bars = {
      makeBar(20240110, "100", "100"),
      makeBar(20240111, "100", "100"),
      makeBar(20240112, "40", "40"),
  };
  const std::vector<std::int32_t> Dates = {20240110, 20240111, 20240112};
  const auto Tl = makeTimeline(Bars, Dates);

  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs = CostConfig::zero();
  Spec.InitialCash = d("100000");
  Spec.Margin.MaxLeverage = d("2");
  Spec.Margin.MaintenanceMarginRatio = d("0.25");

  const std::vector<JournalEntry> Journal = {
      placeAt(0, 0, marketOrder(Side::Buy, 180)),
  };
  const ReplayResult Res = core::replaySession(Spec, Tl, Journal, 2, 0);
  EXPECT_EQ(Res.Engine.marginCallCount(), 1);
  EXPECT_EQ(Res.Engine.liquidationCount(), 1);
  EXPECT_EQ(Res.Engine.portfolio().lots(0), 0);
  EXPECT_EQ(Res.Engine.equity()[2].Equity, d("-8000.00"));
  EXPECT_EQ(Res.Status, SessionStatus::Bankrupt);
}

} // namespace
