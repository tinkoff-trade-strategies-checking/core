#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "support/fixture1.hpp"

namespace {

using core::Bar;
using core::CancelCmd;
using core::CommandResult;
using core::CostConfig;
using core::Decimal;
using core::Fill;
using core::Instrument;
using core::JournalEntry;
using core::Order;
using core::OrderSpec;
using core::OrderStatus;
using core::OrderType;
using core::PlaceCmd;
using core::Reason;
using core::ReplaceCmd;
using core::ReplayResult;
using core::SessionEngine;
using core::SessionSpec;
using core::SessionStatus;
using core::Side;
using core::Timeframe;
using core::TimeInForce;
using core::Timeline;
using core::TradingDay;

Decimal d(const char *Text) { return Decimal::parse(Text); }

std::shared_ptr<const Timeline> fixtureTimeline() {
  auto F = core::testing::fixture1();
  return std::make_shared<const Timeline>(
      Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                     Timeframe::D1, 20240109, 20240122));
}

SessionSpec fixtureSpec() {
  SessionSpec Spec;
  Spec.Instruments = core::testing::fixture1().Instruments;
  Spec.Costs = CostConfig::defaults();
  Spec.InitialCash = d("1000000");
  return Spec;
}

OrderSpec market(Side Side, std::int64_t Lots, int Instrument = 0) {
  OrderSpec S;
  S.Instrument = Instrument;
  S.OrderSide = Side;
  S.Type = OrderType::Market;
  S.Lots = Lots;
  return S;
}

OrderSpec limit(Side Side, std::int64_t Lots, const char *Price,
                int Instrument = 0) {
  OrderSpec S = market(Side, Lots, Instrument);
  S.Type = OrderType::Limit;
  S.LimitPrice = d(Price);
  S.Tif = TimeInForce::Gtc;
  return S;
}

JournalEntry place(std::int64_t Seq, std::int64_t Step, OrderSpec Spec) {
  JournalEntry E;
  E.Seq = Seq;
  E.Step = Step;
  E.Command = PlaceCmd{std::move(Spec), std::nullopt};
  return E;
}

JournalEntry cancel(std::int64_t Seq, std::int64_t Step, std::int64_t OrderId) {
  JournalEntry E;
  E.Seq = Seq;
  E.Step = Step;
  E.Command = CancelCmd{OrderId};
  return E;
}

JournalEntry replace(std::int64_t Seq, std::int64_t Step, std::int64_t OrderId,
                     OrderSpec Spec) {
  JournalEntry E;
  E.Seq = Seq;
  E.Step = Step;
  E.Command = ReplaceCmd{OrderId, PlaceCmd{std::move(Spec), std::nullopt}};
  return E;
}

ReplayResult replay(const std::vector<JournalEntry> &Journal, std::int64_t UpTo,
                    std::int64_t EventsFrom = 0) {
  return core::replaySession(fixtureSpec(), fixtureTimeline(), Journal, UpTo,
                             EventsFrom);
}

const Order &orderById(const SessionEngine &Engine, std::int64_t Id) {
  for (const Order &O : Engine.orders()) {
    if (O.Id == Id)
      return O;
  }
  throw std::out_of_range("нет заявки");
}

void expectSameFill(const Fill &A, const Fill &B) {
  EXPECT_EQ(A.OrderId, B.OrderId);
  EXPECT_EQ(A.Step, B.Step);
  EXPECT_EQ(A.Instrument, B.Instrument);
  EXPECT_EQ(A.OrderSide, B.OrderSide);
  EXPECT_EQ(A.Lots, B.Lots);
  EXPECT_EQ(A.Price, B.Price);
  EXPECT_EQ(A.BasePrice, B.BasePrice);
  EXPECT_EQ(A.Value, B.Value);
  EXPECT_EQ(A.Commission, B.Commission);
  EXPECT_EQ(A.Fee, B.Fee);
  EXPECT_EQ(A.SpreadCost, B.SpreadCost);
  EXPECT_EQ(A.SlippageCost, B.SlippageCost);
}

void expectSameOrder(const Order &A, const Order &B) {
  EXPECT_EQ(A.Id, B.Id);
  EXPECT_EQ(A.Status, B.Status);
  EXPECT_EQ(A.ReasonCode, B.ReasonCode);
  EXPECT_EQ(A.InvalidField, B.InvalidField);
  EXPECT_EQ(A.PlacedStep, B.PlacedStep);
  EXPECT_EQ(A.ClosedStep, B.ClosedStep);
  EXPECT_EQ(A.FilledLots, B.FilledLots);
  EXPECT_EQ(A.Spec.Instrument, B.Spec.Instrument);
  EXPECT_EQ(A.Spec.OrderSide, B.Spec.OrderSide);
  EXPECT_EQ(A.Spec.Lots, B.Spec.Lots);
}

void expectSameState(const SessionEngine &A, const SessionEngine &B) {
  ASSERT_EQ(A.fills().size(), B.fills().size());
  for (std::size_t I = 0; I < A.fills().size(); ++I)
    expectSameFill(A.fills()[I], B.fills()[I]);
  ASSERT_EQ(A.orders().size(), B.orders().size());
  for (std::size_t I = 0; I < A.orders().size(); ++I)
    expectSameOrder(A.orders()[I], B.orders()[I]);
  ASSERT_EQ(A.equity().size(), B.equity().size());
  for (std::size_t I = 0; I < A.equity().size(); ++I) {
    EXPECT_EQ(A.equity()[I].Step, B.equity()[I].Step);
    EXPECT_EQ(A.equity()[I].Equity, B.equity()[I].Equity);
  }
  EXPECT_EQ(A.portfolio().cash(), B.portfolio().cash());
  EXPECT_EQ(A.positionValues(), B.positionValues());
}

// Журнал из 6 записей на фикстуре: подача, ожидание бара бумаги B (на шаге 3
// его нет), лимитная заявка и её отмена.
std::vector<JournalEntry> sixEntries() {
  return {
      place(0, 0, market(Side::Buy, 10)), // A: исполнится на шаге 1
      place(1, 2,
            market(Side::Buy, 5,
                   1)), // B: на шаге 3 бара нет, исполнится на шаге 4
      place(2, 3, market(Side::Sell, 5)), // A: шаг 4
      place(3, 3,
            limit(Side::Buy, 5,
                  "90")), // A: не исполнится (цена ниже рынка), id 6
      cancel(4, 5, 6), // снимает лимитную заявку
      place(5, 6, market(Side::Sell, 5, 1)), // B: шаг 7
  };
}

TEST(Replay, NoFutureBars) {
  const auto Journal =
      std::vector<JournalEntry>{place(0, 1, market(Side::Buy, 10))};
  const ReplayResult R = replay(Journal, 3);
  EXPECT_EQ(R.LastStep, 3);
  ASSERT_EQ(R.Events.size(), 4u);
  for (const auto &E : R.Events) {
    EXPECT_LE(E.Step, 3);
    for (const auto &Bar : E.Bars)
      EXPECT_LE(Bar.Step, 3);
    for (const auto &F : E.Fills)
      EXPECT_LE(F.Step, 3);
    for (const auto &O : E.ChangedOrders)
      EXPECT_LE(O.PlacedStep, 3);
  }
  for (const auto &F : R.Engine.fills())
    EXPECT_LE(F.Step, 3);
  EXPECT_EQ(R.Engine.equity().size(), 4u);
  EXPECT_EQ(R.Engine.positionValues().size(), 4u);
  EXPECT_EQ(R.Engine.currentStep(), 3);
  EXPECT_EQ(R.Engine.lastStepEvents().Step, 3);
}

TEST(Replay, OrderExecutesNextBar) {
  const ReplayResult R = replay({place(0, 2, market(Side::Buy, 10))}, 5);
  ASSERT_EQ(R.Engine.fills().size(), 1u);
  EXPECT_EQ(R.Engine.fills()[0].Step, 3);
  EXPECT_EQ(R.Engine.fills()[0].BasePrice,
            d("103")); // open шага 3 = close шага 2
  const Order &O = orderById(R.Engine, 0);
  EXPECT_EQ(O.Status, OrderStatus::Filled);
  EXPECT_EQ(O.PlacedStep, 2);
  // До шага 3 исполнения нет.
  const ReplayResult Before = replay({place(0, 2, market(Side::Buy, 10))}, 2);
  EXPECT_TRUE(Before.Engine.fills().empty());
  EXPECT_EQ(orderById(Before.Engine, 0).Status, OrderStatus::Active);
}

TEST(Replay, Deterministic) {
  const auto Journal = sixEntries();
  const ReplayResult Full = replay(Journal, 9);
  ASSERT_EQ(Full.CommandResults.size(), 6u);
  for (const CommandResult &C : Full.CommandResults)
    EXPECT_TRUE(C.Accepted) << C.Seq;
  EXPECT_FALSE(Full.Engine.fills().empty());

  // Повторный проигрыш того же журнала.
  expectSameState(Full.Engine, replay(Journal, 9).Engine);

  // Вручную по шагам: apply + advance.
  SessionEngine Live(fixtureSpec(), fixtureTimeline());
  std::size_t Next = 0;
  for (std::int64_t K = 0;; ++K) {
    while (Next < Journal.size() && Journal[Next].Step == K)
      Live.apply(Journal[Next++]);
    if (K == 9)
      break;
    Live.advance();
  }
  expectSameState(Full.Engine, Live);

  // «По шагам»: каждый раз заново до k; кривая стоимости и исполнения - префикс
  // полного прогона.
  for (std::int64_t K = 0; K <= 9; ++K) {
    std::vector<JournalEntry> Prefix;
    for (const auto &E : Journal) {
      if (E.Step <= K)
        Prefix.push_back(E);
    }
    const ReplayResult Part = replay(Prefix, K);
    ASSERT_EQ(Part.Engine.equity().size(), static_cast<std::size_t>(K + 1));
    for (std::int64_t S = 0; S <= K; ++S) {
      EXPECT_EQ(Part.Engine.equity()[static_cast<std::size_t>(S)].Equity,
                Full.Engine.equity()[static_cast<std::size_t>(S)].Equity)
          << "k=" << K << " s=" << S;
    }
    std::size_t Expected = 0;
    for (const Fill &F : Full.Engine.fills()) {
      if (F.Step <= K)
        ++Expected;
    }
    ASSERT_EQ(Part.Engine.fills().size(), Expected) << "k=" << K;
    for (std::size_t I = 0; I < Expected; ++I)
      expectSameFill(Part.Engine.fills()[I], Full.Engine.fills()[I]);
  }
}

// INV-8: восстановление после обрыва - проиграть журнал до k и продолжить с
// того же места.
TEST(Replay, ResumeFromReplayedEngineEqualsFullReplay) {
  const auto Journal = sixEntries();
  const ReplayResult Full = replay(Journal, 9);
  for (std::int64_t K = 0; K <= 8; ++K) {
    std::vector<JournalEntry> Prefix;
    for (const auto &E : Journal) {
      if (E.Step <= K)
        Prefix.push_back(E);
    }
    SessionEngine Resumed = replay(Prefix, K).Engine;
    std::size_t Next = Prefix.size();
    for (std::int64_t S = K;; ++S) {
      while (Next < Journal.size() && Journal[Next].Step == S)
        Resumed.apply(Journal[Next++]);
      if (S == 9)
        break;
      Resumed.advance();
    }
    SCOPED_TRACE(K);
    expectSameState(Full.Engine, Resumed);
  }
}

TEST(Replay, RejectedStillJournaled) {
  const ReplayResult R = replay({place(0, 1, market(Side::Sell, 5))}, 3);
  ASSERT_EQ(R.CommandResults.size(), 1u);
  const CommandResult &C = R.CommandResults[0];
  EXPECT_FALSE(C.Accepted);
  EXPECT_EQ(C.ReasonCode, Reason::InsufficientPosition);
  ASSERT_EQ(C.OrderIds.size(), 1u);
  EXPECT_EQ(C.OrderIds[0], 0);
  const Order &O = orderById(R.Engine, 0);
  EXPECT_EQ(O.Status, OrderStatus::Rejected);
  EXPECT_EQ(O.ReasonCode, Reason::InsufficientPosition);
  EXPECT_TRUE(R.Engine.fills().empty());
  // Отказ попал в события шага подачи.
  const ReplayResult At = replay({place(0, 1, market(Side::Sell, 5))}, 1, 1);
  ASSERT_EQ(At.Events.size(), 1u);
  ASSERT_EQ(At.Events[0].ChangedOrders.size(), 1u);
  EXPECT_EQ(At.Events[0].ChangedOrders[0].Status, OrderStatus::Rejected);
}

TEST(Replay, InvalidParamsCarriesField) {
  const ReplayResult R = replay({place(0, 1, market(Side::Buy, 0))}, 1);
  EXPECT_FALSE(R.CommandResults[0].Accepted);
  EXPECT_EQ(R.CommandResults[0].ReasonCode, Reason::InvalidParams);
  EXPECT_EQ(R.CommandResults[0].Field, "lots");
  EXPECT_EQ(orderById(R.Engine, 0).InvalidField, "lots");
}

TEST(Replay, ReplaceAtomic) {
  const std::vector<JournalEntry> Journal = {
      place(0, 1, limit(Side::Buy, 5, "90")),
      replace(1, 2, 0, limit(Side::Buy, 0, "91")), // новая заявка некорректна
  };
  const ReplayResult R = replay(Journal, 3);
  EXPECT_TRUE(R.CommandResults[0].Accepted);
  EXPECT_FALSE(R.CommandResults[1].Accepted);
  EXPECT_EQ(R.CommandResults[1].ReasonCode, Reason::InvalidParams);
  EXPECT_EQ(R.CommandResults[1].Field, "lots");
  EXPECT_EQ(orderById(R.Engine, 0).Status,
            OrderStatus::Active); // старая не тронута
  EXPECT_EQ(orderById(R.Engine, 2).Status, OrderStatus::Rejected);

  // Корректная замена: старая снята с Replaced, новая активна.
  const ReplayResult Ok = replay({place(0, 1, limit(Side::Buy, 5, "90")),
                                  replace(1, 2, 0, limit(Side::Buy, 4, "91"))},
                                 3);
  EXPECT_TRUE(Ok.CommandResults[1].Accepted);
  EXPECT_EQ(orderById(Ok.Engine, 0).Status, OrderStatus::Cancelled);
  EXPECT_EQ(orderById(Ok.Engine, 0).ReasonCode, Reason::Replaced);
  EXPECT_EQ(orderById(Ok.Engine, 2).Status, OrderStatus::Active);
  EXPECT_EQ(orderById(Ok.Engine, 2).Spec.Lots, 4);

  // Замена неактивной заявки отклоняется.
  const ReplayResult Dead =
      replay({place(0, 1, limit(Side::Buy, 5, "90")), cancel(1, 2, 0),
              replace(2, 3, 0, limit(Side::Buy, 4, "91"))},
             3);
  EXPECT_FALSE(Dead.CommandResults[2].Accepted);
  EXPECT_EQ(Dead.CommandResults[2].ReasonCode, Reason::OrderNotActive);
}

TEST(Replay, ReplaceOcoReplacesWholePair) {
  JournalEntry Pair;
  Pair.Seq = 0;
  Pair.Step = 1;
  Pair.Command =
      PlaceCmd{limit(Side::Buy, 5, "90"), limit(Side::Buy, 3, "0.01", 1)};
  const ReplayResult Placed = replay({Pair}, 1);
  ASSERT_EQ(Placed.CommandResults[0].OrderIds,
            (std::vector<std::int64_t>{0, 1}));
  EXPECT_EQ(orderById(Placed.Engine, 0).OcoSiblingId, 1);
  EXPECT_EQ(orderById(Placed.Engine, 1).OcoSiblingId, 0);

  const ReplayResult R =
      replay({Pair, replace(1, 2, 0, limit(Side::Buy, 4, "91"))}, 2);
  EXPECT_TRUE(R.CommandResults[1].Accepted);
  EXPECT_EQ(orderById(R.Engine, 0).Status, OrderStatus::Cancelled);
  EXPECT_EQ(orderById(R.Engine, 1).Status, OrderStatus::Cancelled);
  EXPECT_EQ(orderById(R.Engine, 2).Status, OrderStatus::Active);
}

TEST(Replay, OrderIdIsSeqTimesTwoPlusLeg) {
  // Две заявки на одном шаге - норма; id = seq·2.
  const ReplayResult R = replay(
      {place(3, 1, market(Side::Buy, 1)), place(7, 1, market(Side::Buy, 1))},
      2);
  EXPECT_EQ(R.Engine.orders()[0].Id, 6);
  EXPECT_EQ(R.Engine.orders()[1].Id, 14);
  EXPECT_EQ(R.Engine.fills().size(), 2u);
}

TEST(Replay, FinishedEndCancelsOrders) {
  const std::vector<JournalEntry> Journal = {
      place(0, 1, limit(Side::Buy, 5, "50"))};
  const ReplayResult R = replay(Journal, 9);
  EXPECT_EQ(R.Status, SessionStatus::FinishedEnd);
  EXPECT_EQ(R.LastStep, 9);
  const Order &O = orderById(R.Engine, 0);
  EXPECT_EQ(O.Status, OrderStatus::Cancelled);
  EXPECT_EQ(O.ReasonCode, Reason::SessionEnd);
  EXPECT_EQ(O.ClosedStep, 9);
  // Команда после остановки - отказ SessionNotActive.
  std::vector<JournalEntry> Late = Journal;
  Late.push_back(place(1, 9, market(Side::Buy, 1)));
  const ReplayResult L = replay(Late, 9);
  EXPECT_EQ(L.CommandResults[1].ReasonCode, Reason::SessionNotActive);
  EXPECT_FALSE(L.CommandResults[1].Accepted);
  // До последнего шага сессия идёт.
  EXPECT_EQ(replay(Journal, 8).Status, SessionStatus::Running);
}

TEST(Replay, EquitySeriesLength) {
  for (const std::int64_t UpTo : {0, 1, 4, 9}) {
    const ReplayResult R = replay({place(0, 0, market(Side::Buy, 10))}, UpTo);
    EXPECT_EQ(R.Engine.equity().size(),
              static_cast<std::size_t>(R.LastStep + 1));
    EXPECT_EQ(R.Engine.positionValues().size(),
              static_cast<std::size_t>(R.LastStep + 1));
    for (std::int64_t S = 0; S <= R.LastStep; ++S)
      EXPECT_EQ(R.Engine.equity()[static_cast<std::size_t>(S)].Step, S);
  }
}

TEST(Replay, EquityAndPositionValues) {
  const ReplayResult R = replay({place(0, 0, market(Side::Buy, 10))}, 2);
  EXPECT_EQ(R.Engine.equity()[0].Equity,
            d("1000000")); // шаг 0: заявок нет, стоимость = капитал
  EXPECT_EQ(R.Engine.positionValues()[0],
            (std::vector<Decimal>{d("0"), d("0")}));
  ASSERT_EQ(R.Engine.fills().size(), 1u);
  EXPECT_EQ(R.Engine.fills()[0].Step, 1);
  // 10 лотов по 10 штук, close шага 1 = 101 и шага 2 = 103.
  EXPECT_EQ(R.Engine.positionValues()[1][0], d("10100"));
  EXPECT_EQ(R.Engine.positionValues()[2][0], d("10300"));
  const auto Snap = R.Engine.snapshot();
  EXPECT_EQ(Snap.Lots[0], 10);
  EXPECT_EQ(Snap.MarketValue[0], d("10300"));
  EXPECT_EQ(Snap.Cash + Snap.MarketValue[0] + Snap.MarketValue[1], Snap.Equity);
  EXPECT_EQ(Snap.Equity, R.Engine.equity()[2].Equity);
}

TEST(Replay, OrderWaitsForOwnInstrumentBar) {
  // У бумаги B на шаге 3 бара нет: заявка, поданная на шаге 2, ждёт шага 4.
  const ReplayResult R = replay({place(0, 2, market(Side::Buy, 5, 1))}, 5);
  ASSERT_EQ(R.Engine.fills().size(), 1u);
  EXPECT_EQ(R.Engine.fills()[0].Step, 4);
}

TEST(Replay, EventsFromStep) {
  const ReplayResult R = replay({place(0, 1, market(Side::Buy, 10))}, 5, 3);
  ASSERT_EQ(R.Events.size(), 3u);
  EXPECT_EQ(R.Events.front().Step, 3);
  EXPECT_EQ(R.Events.back().Step, 5);
  // События шага 2 содержат исполнение рыночной заявки, поданной на шаге 1.
  const ReplayResult All = replay({place(0, 1, market(Side::Buy, 10))}, 5, 0);
  ASSERT_EQ(All.Events.size(), 6u);
  EXPECT_EQ(All.Events[2].Fills.size(), 1u);
  EXPECT_EQ(All.Events[2].ChangedOrders.size(), 1u);
  EXPECT_EQ(All.Events[2].Portfolio.Lots[0], 10);
  EXPECT_EQ(All.Events[1].ChangedOrders.size(), 1u); // подача на шаге 1
}

TEST(Replay, InvalidJournal) {
  EXPECT_THROW(replay({place(0, 2, market(Side::Buy, 1)),
                       place(1, 1, market(Side::Buy, 1))},
                      5),
               std::invalid_argument); // шаг убывает
  EXPECT_THROW(replay({place(0, 4, market(Side::Buy, 1))}, 3),
               std::invalid_argument); // step > upToStep
  EXPECT_THROW(replay({place(1, 1, market(Side::Buy, 1)),
                       place(1, 1, market(Side::Buy, 1))},
                      3),
               std::invalid_argument); // seq повторяется
  EXPECT_THROW(replay({}, 10), std::invalid_argument); // шага 10 нет
  EXPECT_THROW(replay({}, -1), std::invalid_argument);
  // Равные шаги допустимы.
  EXPECT_NO_THROW(replay(
      {place(0, 1, market(Side::Buy, 1)), place(1, 1, market(Side::Buy, 1))},
      3));
}

TEST(SessionEngine, ApplyChecksStep) {
  SessionEngine Engine(fixtureSpec(), fixtureTimeline());
  EXPECT_THROW(Engine.apply(place(0, 1, market(Side::Buy, 1))),
               std::invalid_argument);
  Engine.advance();
  EXPECT_THROW(Engine.apply(place(0, 0, market(Side::Buy, 1))),
               std::invalid_argument);
  EXPECT_NO_THROW(Engine.apply(place(0, 1, market(Side::Buy, 1))));
  EXPECT_THROW(Engine.apply(place(0, 1, market(Side::Buy, 1))),
               std::invalid_argument); // seq не растёт
}

TEST(SessionEngine, RejectsBadSpec) {
  SessionSpec Spec = fixtureSpec();
  Spec.InitialCash = d("0");
  EXPECT_THROW(SessionEngine(Spec, fixtureTimeline()), std::invalid_argument);
  Spec = fixtureSpec();
  Spec.Instruments.pop_back();
  EXPECT_THROW(SessionEngine(Spec, fixtureTimeline()), std::invalid_argument);
}

TEST(SessionEngine, CopyIsSnapshot) {
  SessionEngine Engine(fixtureSpec(), fixtureTimeline());
  Engine.apply(place(0, 0, market(Side::Buy, 10)));
  const SessionEngine Snapshot = Engine;
  Engine.advance();
  Engine.advance();
  EXPECT_EQ(Snapshot.currentStep(), 0);
  EXPECT_TRUE(Snapshot.fills().empty());
  EXPECT_EQ(Snapshot.equity().size(), 1u);
  EXPECT_EQ(Engine.fills().size(), 1u);
  SessionEngine Fork = Snapshot; // из снимка можно идти дальше независимо
  Fork.advance();
  EXPECT_EQ(Fork.fills().size(), 1u);
  EXPECT_EQ(Snapshot.fills().size(), 0u);
}

TEST(SessionEngine, AdvanceAfterStopThrows) {
  const ReplayResult R = replay({}, 9);
  SessionEngine Engine = R.Engine;
  EXPECT_THROW(Engine.advance(), std::logic_error);
}

TEST(Replay, CancelOcoLegCancelsPair) {
  JournalEntry Pair;
  Pair.Seq = 0;
  Pair.Step = 1;
  Pair.Command =
      PlaceCmd{limit(Side::Buy, 5, "90"), limit(Side::Buy, 3, "0.01", 1)};
  const ReplayResult R = replay({Pair, cancel(1, 2, 1)}, 3);
  EXPECT_TRUE(R.CommandResults[1].Accepted);
  EXPECT_EQ(orderById(R.Engine, 0).Status, OrderStatus::Cancelled);
  EXPECT_EQ(orderById(R.Engine, 0).ReasonCode, Reason::CancelledByUser);
  EXPECT_EQ(orderById(R.Engine, 1).Status, OrderStatus::Cancelled);
  EXPECT_EQ(orderById(R.Engine, 1).ClosedStep, 2);
  // Повторная отмена - OrderNotActive.
  const ReplayResult Again =
      replay({Pair, cancel(1, 2, 1), cancel(2, 2, 0)}, 3);
  EXPECT_FALSE(Again.CommandResults[2].Accepted);
  EXPECT_EQ(Again.CommandResults[2].ReasonCode, Reason::OrderNotActive);
}

// Одна бумага, лот 1, шаг цены 0.01, 4 дня; close по списку (open = close
// предыдущего бара).
std::shared_ptr<const Timeline>
singleTimeline(const std::vector<const char *> &Closes) {
  const std::int64_t DayMs = 86'400'000;
  const std::int64_t Start = 1'704'747'600'000; // 2024-01-09 00:00 МСК
  std::vector<Bar> Bars;
  std::vector<TradingDay> Calendar;
  Decimal Prev = d(Closes[0]);
  for (std::size_t I = 0; I < Closes.size(); ++I) {
    Bar B;
    B.TsOpenMs = Start + static_cast<std::int64_t>(I) * DayMs;
    B.AvailableAtMs = B.TsOpenMs + DayMs;
    B.Open = Prev;
    B.Close = d(Closes[I]);
    B.High = std::max(B.Open, B.Close);
    B.Low = std::min(B.Open, B.Close);
    B.Volume = 1000;
    Prev = B.Close;
    Bars.push_back(B);
    Calendar.push_back(
        TradingDay{static_cast<std::int32_t>(20240109 + I), true});
  }
  const std::vector<Instrument> Instruments = {
      Instrument{"A", "SOLO", 1, d("0.001")}};
  return std::make_shared<const Timeline>(
      Timeline::build(Instruments, {Bars}, Calendar));
}

TEST(Replay, BankruptStopsSession) {
  SessionSpec Spec;
  Spec.Instruments = {Instrument{"A", "SOLO", 1, d("0.001")}};
  Spec.Costs = CostConfig::zero();
  Spec.InitialCash = d("0.99");
  const auto Timeline =
      singleTimeline({"0.99", "0.99", "0.001", "0.001", "0.001"});
  const std::vector<JournalEntry> Journal = {place(0, 0, market(Side::Buy, 1))};
  // Покупка на весь капитал, затем цена падает так, что позиция стоит 0,00 ->
  // капитал 0.
  const ReplayResult R = core::replaySession(Spec, Timeline, Journal, 4, 0);
  EXPECT_EQ(R.Status, SessionStatus::Bankrupt);
  EXPECT_EQ(R.LastStep, 2);
  EXPECT_EQ(R.Engine.equity().size(), 3u);
  EXPECT_EQ(R.Events.size(), 3u);
  EXPECT_EQ(R.Engine.equity()[1].Equity, d("0.99"));
  EXPECT_TRUE(R.Engine.equity()[2].Equity.isZero());
  // Прогон до шага 1 ещё идёт.
  EXPECT_EQ(core::replaySession(Spec, Timeline, Journal, 1, 0).Status,
            SessionStatus::Running);
  // Запись после банкротства - отказ SessionNotActive, журнал остаётся
  // проигрываемым.
  const ReplayResult Late = core::replaySession(
      Spec, Timeline,
      std::vector<JournalEntry>{place(0, 0, market(Side::Buy, 1)),
                                place(1, 3, market(Side::Buy, 1))},
      4, 0);
  ASSERT_EQ(Late.CommandResults.size(), 2u);
  EXPECT_FALSE(Late.CommandResults[1].Accepted);
  EXPECT_EQ(Late.CommandResults[1].ReasonCode, Reason::SessionNotActive);
  EXPECT_EQ(Late.LastStep, 2);
  EXPECT_EQ(Late.Engine.orders().back().Status, OrderStatus::Rejected);
  EXPECT_EQ(Late.Events.back().ChangedOrders.back().Id,
            2); // отказ виден в событиях последнего шага
}

} // namespace
