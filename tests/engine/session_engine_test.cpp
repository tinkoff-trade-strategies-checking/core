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

Decimal D(const char *text) { return Decimal::parse(text); }

std::shared_ptr<const Timeline> fixtureTimeline() {
  auto f = core::testing::fixture1();
  return std::make_shared<const Timeline>(
      Timeline::load(f.bars, f.calendar, f.snapshotId, f.instruments,
                     Timeframe::D1, 20240109, 20240122));
}

SessionSpec fixtureSpec(bool naive = false) {
  SessionSpec spec;
  spec.instruments = core::testing::fixture1().instruments;
  spec.costs = CostConfig::defaults();
  spec.initialCash = D("1000000");
  spec.naive = naive;
  return spec;
}

OrderSpec market(Side side, std::int64_t lots, int instrument = 0) {
  OrderSpec s;
  s.instrument = instrument;
  s.side = side;
  s.type = OrderType::Market;
  s.lots = lots;
  return s;
}

OrderSpec limit(Side side, std::int64_t lots, const char *price,
                int instrument = 0) {
  OrderSpec s = market(side, lots, instrument);
  s.type = OrderType::Limit;
  s.limitPrice = D(price);
  s.tif = TimeInForce::Gtc;
  return s;
}

JournalEntry place(std::int64_t seq, std::int64_t step, OrderSpec spec) {
  JournalEntry e;
  e.seq = seq;
  e.step = step;
  e.command = PlaceCmd{std::move(spec), std::nullopt};
  return e;
}

JournalEntry cancel(std::int64_t seq, std::int64_t step, std::int64_t orderId) {
  JournalEntry e;
  e.seq = seq;
  e.step = step;
  e.command = CancelCmd{orderId};
  return e;
}

JournalEntry replace(std::int64_t seq, std::int64_t step, std::int64_t orderId,
                     OrderSpec spec) {
  JournalEntry e;
  e.seq = seq;
  e.step = step;
  e.command = ReplaceCmd{orderId, PlaceCmd{std::move(spec), std::nullopt}};
  return e;
}

ReplayResult replay(const std::vector<JournalEntry> &journal, std::int64_t upTo,
                    std::int64_t eventsFrom = 0, bool naive = false) {
  return core::replaySession(fixtureSpec(naive), fixtureTimeline(), journal,
                             upTo, eventsFrom);
}

const Order &orderById(const SessionEngine &engine, std::int64_t id) {
  for (const Order &o : engine.orders()) {
    if (o.id == id)
      return o;
  }
  throw std::out_of_range("нет заявки");
}

void expectSameFill(const Fill &a, const Fill &b) {
  EXPECT_EQ(a.orderId, b.orderId);
  EXPECT_EQ(a.step, b.step);
  EXPECT_EQ(a.instrument, b.instrument);
  EXPECT_EQ(a.side, b.side);
  EXPECT_EQ(a.lots, b.lots);
  EXPECT_EQ(a.price, b.price);
  EXPECT_EQ(a.basePrice, b.basePrice);
  EXPECT_EQ(a.value, b.value);
  EXPECT_EQ(a.commission, b.commission);
  EXPECT_EQ(a.fee, b.fee);
  EXPECT_EQ(a.spreadCost, b.spreadCost);
  EXPECT_EQ(a.slippageCost, b.slippageCost);
}

void expectSameOrder(const Order &a, const Order &b) {
  EXPECT_EQ(a.id, b.id);
  EXPECT_EQ(a.status, b.status);
  EXPECT_EQ(a.reason, b.reason);
  EXPECT_EQ(a.invalidField, b.invalidField);
  EXPECT_EQ(a.placedStep, b.placedStep);
  EXPECT_EQ(a.closedStep, b.closedStep);
  EXPECT_EQ(a.filledLots, b.filledLots);
  EXPECT_EQ(a.spec.instrument, b.spec.instrument);
  EXPECT_EQ(a.spec.side, b.spec.side);
  EXPECT_EQ(a.spec.lots, b.spec.lots);
}

void expectSameState(const SessionEngine &a, const SessionEngine &b) {
  ASSERT_EQ(a.fills().size(), b.fills().size());
  for (std::size_t i = 0; i < a.fills().size(); ++i)
    expectSameFill(a.fills()[i], b.fills()[i]);
  ASSERT_EQ(a.orders().size(), b.orders().size());
  for (std::size_t i = 0; i < a.orders().size(); ++i)
    expectSameOrder(a.orders()[i], b.orders()[i]);
  ASSERT_EQ(a.equity().size(), b.equity().size());
  for (std::size_t i = 0; i < a.equity().size(); ++i) {
    EXPECT_EQ(a.equity()[i].step, b.equity()[i].step);
    EXPECT_EQ(a.equity()[i].equity, b.equity()[i].equity);
  }
  EXPECT_EQ(a.portfolio().cash(), b.portfolio().cash());
  EXPECT_EQ(a.positionValues(), b.positionValues());
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
  const auto journal =
      std::vector<JournalEntry>{place(0, 1, market(Side::Buy, 10))};
  const ReplayResult r = replay(journal, 3);
  EXPECT_EQ(r.lastStep, 3);
  ASSERT_EQ(r.events.size(), 4u);
  for (const auto &e : r.events) {
    EXPECT_LE(e.step, 3);
    for (const auto &bar : e.bars)
      EXPECT_LE(bar.step, 3);
    for (const auto &f : e.fills)
      EXPECT_LE(f.step, 3);
    for (const auto &o : e.changedOrders)
      EXPECT_LE(o.placedStep, 3);
  }
  for (const auto &f : r.engine.fills())
    EXPECT_LE(f.step, 3);
  EXPECT_EQ(r.engine.equity().size(), 4u);
  EXPECT_EQ(r.engine.positionValues().size(), 4u);
  EXPECT_EQ(r.engine.currentStep(), 3);
  EXPECT_EQ(r.engine.lastStepEvents().step, 3);
}

TEST(Replay, OrderExecutesNextBar) {
  const ReplayResult r = replay({place(0, 2, market(Side::Buy, 10))}, 5);
  ASSERT_EQ(r.engine.fills().size(), 1u);
  EXPECT_EQ(r.engine.fills()[0].step, 3);
  EXPECT_EQ(r.engine.fills()[0].basePrice,
            D("103")); // open шага 3 = close шага 2
  const Order &o = orderById(r.engine, 0);
  EXPECT_EQ(o.status, OrderStatus::Filled);
  EXPECT_EQ(o.placedStep, 2);
  // До шага 3 исполнения нет.
  const ReplayResult before = replay({place(0, 2, market(Side::Buy, 10))}, 2);
  EXPECT_TRUE(before.engine.fills().empty());
  EXPECT_EQ(orderById(before.engine, 0).status, OrderStatus::Active);
}

TEST(Replay, Deterministic) {
  const auto journal = sixEntries();
  const ReplayResult full = replay(journal, 9);
  ASSERT_EQ(full.commandResults.size(), 6u);
  for (const CommandResult &c : full.commandResults)
    EXPECT_TRUE(c.accepted) << c.seq;
  EXPECT_FALSE(full.engine.fills().empty());

  // Повторный проигрыш того же журнала.
  expectSameState(full.engine, replay(journal, 9).engine);

  // Вручную по шагам: apply + advance.
  SessionEngine live(fixtureSpec(), fixtureTimeline());
  std::size_t next = 0;
  for (std::int64_t k = 0;; ++k) {
    while (next < journal.size() && journal[next].step == k)
      live.apply(journal[next++]);
    if (k == 9)
      break;
    live.advance();
  }
  expectSameState(full.engine, live);

  // «По шагам»: каждый раз заново до k; кривая стоимости и исполнения - префикс
  // полного прогона.
  for (std::int64_t k = 0; k <= 9; ++k) {
    std::vector<JournalEntry> prefix;
    for (const auto &e : journal) {
      if (e.step <= k)
        prefix.push_back(e);
    }
    const ReplayResult part = replay(prefix, k);
    ASSERT_EQ(part.engine.equity().size(), static_cast<std::size_t>(k + 1));
    for (std::int64_t s = 0; s <= k; ++s) {
      EXPECT_EQ(part.engine.equity()[static_cast<std::size_t>(s)].equity,
                full.engine.equity()[static_cast<std::size_t>(s)].equity)
          << "k=" << k << " s=" << s;
    }
    std::size_t expected = 0;
    for (const Fill &f : full.engine.fills()) {
      if (f.step <= k)
        ++expected;
    }
    ASSERT_EQ(part.engine.fills().size(), expected) << "k=" << k;
    for (std::size_t i = 0; i < expected; ++i)
      expectSameFill(part.engine.fills()[i], full.engine.fills()[i]);
  }
}

// INV-8: восстановление после обрыва - проиграть журнал до k и продолжить с
// того же места.
TEST(Replay, ResumeFromReplayedEngineEqualsFullReplay) {
  const auto journal = sixEntries();
  const ReplayResult full = replay(journal, 9);
  for (std::int64_t k = 0; k <= 8; ++k) {
    std::vector<JournalEntry> prefix;
    for (const auto &e : journal) {
      if (e.step <= k)
        prefix.push_back(e);
    }
    SessionEngine resumed = replay(prefix, k).engine;
    std::size_t next = prefix.size();
    for (std::int64_t s = k;; ++s) {
      while (next < journal.size() && journal[next].step == s)
        resumed.apply(journal[next++]);
      if (s == 9)
        break;
      resumed.advance();
    }
    SCOPED_TRACE(k);
    expectSameState(full.engine, resumed);
  }
}

TEST(Replay, RejectedStillJournaled) {
  const ReplayResult r = replay({place(0, 1, market(Side::Sell, 5))}, 3);
  ASSERT_EQ(r.commandResults.size(), 1u);
  const CommandResult &c = r.commandResults[0];
  EXPECT_FALSE(c.accepted);
  EXPECT_EQ(c.reason, Reason::InsufficientPosition);
  ASSERT_EQ(c.orderIds.size(), 1u);
  EXPECT_EQ(c.orderIds[0], 0);
  const Order &o = orderById(r.engine, 0);
  EXPECT_EQ(o.status, OrderStatus::Rejected);
  EXPECT_EQ(o.reason, Reason::InsufficientPosition);
  EXPECT_TRUE(r.engine.fills().empty());
  // Отказ попал в события шага подачи.
  const ReplayResult at = replay({place(0, 1, market(Side::Sell, 5))}, 1, 1);
  ASSERT_EQ(at.events.size(), 1u);
  ASSERT_EQ(at.events[0].changedOrders.size(), 1u);
  EXPECT_EQ(at.events[0].changedOrders[0].status, OrderStatus::Rejected);
}

TEST(Replay, InvalidParamsCarriesField) {
  const ReplayResult r = replay({place(0, 1, market(Side::Buy, 0))}, 1);
  EXPECT_FALSE(r.commandResults[0].accepted);
  EXPECT_EQ(r.commandResults[0].reason, Reason::InvalidParams);
  EXPECT_EQ(r.commandResults[0].field, "lots");
  EXPECT_EQ(orderById(r.engine, 0).invalidField, "lots");
}

TEST(Replay, ReplaceAtomic) {
  const std::vector<JournalEntry> journal = {
      place(0, 1, limit(Side::Buy, 5, "90")),
      replace(1, 2, 0, limit(Side::Buy, 0, "91")), // новая заявка некорректна
  };
  const ReplayResult r = replay(journal, 3);
  EXPECT_TRUE(r.commandResults[0].accepted);
  EXPECT_FALSE(r.commandResults[1].accepted);
  EXPECT_EQ(r.commandResults[1].reason, Reason::InvalidParams);
  EXPECT_EQ(r.commandResults[1].field, "lots");
  EXPECT_EQ(orderById(r.engine, 0).status,
            OrderStatus::Active); // старая не тронута
  EXPECT_EQ(orderById(r.engine, 2).status, OrderStatus::Rejected);

  // Корректная замена: старая снята с Replaced, новая активна.
  const ReplayResult ok = replay({place(0, 1, limit(Side::Buy, 5, "90")),
                                  replace(1, 2, 0, limit(Side::Buy, 4, "91"))},
                                 3);
  EXPECT_TRUE(ok.commandResults[1].accepted);
  EXPECT_EQ(orderById(ok.engine, 0).status, OrderStatus::Cancelled);
  EXPECT_EQ(orderById(ok.engine, 0).reason, Reason::Replaced);
  EXPECT_EQ(orderById(ok.engine, 2).status, OrderStatus::Active);
  EXPECT_EQ(orderById(ok.engine, 2).spec.lots, 4);

  // Замена неактивной заявки отклоняется.
  const ReplayResult dead =
      replay({place(0, 1, limit(Side::Buy, 5, "90")), cancel(1, 2, 0),
              replace(2, 3, 0, limit(Side::Buy, 4, "91"))},
             3);
  EXPECT_FALSE(dead.commandResults[2].accepted);
  EXPECT_EQ(dead.commandResults[2].reason, Reason::OrderNotActive);
}

TEST(Replay, ReplaceOcoReplacesWholePair) {
  JournalEntry pair;
  pair.seq = 0;
  pair.step = 1;
  pair.command =
      PlaceCmd{limit(Side::Buy, 5, "90"), limit(Side::Buy, 3, "0.01", 1)};
  const ReplayResult placed = replay({pair}, 1);
  ASSERT_EQ(placed.commandResults[0].orderIds,
            (std::vector<std::int64_t>{0, 1}));
  EXPECT_EQ(orderById(placed.engine, 0).ocoSiblingId, 1);
  EXPECT_EQ(orderById(placed.engine, 1).ocoSiblingId, 0);

  const ReplayResult r =
      replay({pair, replace(1, 2, 0, limit(Side::Buy, 4, "91"))}, 2);
  EXPECT_TRUE(r.commandResults[1].accepted);
  EXPECT_EQ(orderById(r.engine, 0).status, OrderStatus::Cancelled);
  EXPECT_EQ(orderById(r.engine, 1).status, OrderStatus::Cancelled);
  EXPECT_EQ(orderById(r.engine, 2).status, OrderStatus::Active);
}

TEST(Replay, OrderIdIsSeqTimesTwoPlusLeg) {
  // Две заявки на одном шаге - норма; id = seq·2.
  const ReplayResult r = replay(
      {place(3, 1, market(Side::Buy, 1)), place(7, 1, market(Side::Buy, 1))},
      2);
  EXPECT_EQ(r.engine.orders()[0].id, 6);
  EXPECT_EQ(r.engine.orders()[1].id, 14);
  EXPECT_EQ(r.engine.fills().size(), 2u);
}

TEST(Replay, FinishedEndCancelsOrders) {
  const std::vector<JournalEntry> journal = {
      place(0, 1, limit(Side::Buy, 5, "50"))};
  const ReplayResult r = replay(journal, 9);
  EXPECT_EQ(r.status, SessionStatus::FinishedEnd);
  EXPECT_EQ(r.lastStep, 9);
  const Order &o = orderById(r.engine, 0);
  EXPECT_EQ(o.status, OrderStatus::Cancelled);
  EXPECT_EQ(o.reason, Reason::SessionEnd);
  EXPECT_EQ(o.closedStep, 9);
  // Команда после остановки - отказ SessionNotActive.
  std::vector<JournalEntry> late = journal;
  late.push_back(place(1, 9, market(Side::Buy, 1)));
  const ReplayResult l = replay(late, 9);
  EXPECT_EQ(l.commandResults[1].reason, Reason::SessionNotActive);
  EXPECT_FALSE(l.commandResults[1].accepted);
  // До последнего шага сессия идёт.
  EXPECT_EQ(replay(journal, 8).status, SessionStatus::Running);
}

TEST(Replay, EquitySeriesLength) {
  for (const std::int64_t upTo : {0, 1, 4, 9}) {
    const ReplayResult r = replay({place(0, 0, market(Side::Buy, 10))}, upTo);
    EXPECT_EQ(r.engine.equity().size(),
              static_cast<std::size_t>(r.lastStep + 1));
    EXPECT_EQ(r.engine.positionValues().size(),
              static_cast<std::size_t>(r.lastStep + 1));
    for (std::int64_t s = 0; s <= r.lastStep; ++s)
      EXPECT_EQ(r.engine.equity()[static_cast<std::size_t>(s)].step, s);
  }
}

TEST(Replay, EquityAndPositionValues) {
  const ReplayResult r = replay({place(0, 0, market(Side::Buy, 10))}, 2);
  EXPECT_EQ(r.engine.equity()[0].equity,
            D("1000000")); // шаг 0: заявок нет, стоимость = капитал
  EXPECT_EQ(r.engine.positionValues()[0],
            (std::vector<Decimal>{D("0"), D("0")}));
  ASSERT_EQ(r.engine.fills().size(), 1u);
  EXPECT_EQ(r.engine.fills()[0].step, 1);
  // 10 лотов по 10 штук, close шага 1 = 101 и шага 2 = 103.
  EXPECT_EQ(r.engine.positionValues()[1][0], D("10100"));
  EXPECT_EQ(r.engine.positionValues()[2][0], D("10300"));
  const auto snap = r.engine.snapshot();
  EXPECT_EQ(snap.lots[0], 10);
  EXPECT_EQ(snap.marketValue[0], D("10300"));
  EXPECT_EQ(snap.cash + snap.marketValue[0] + snap.marketValue[1], snap.equity);
  EXPECT_EQ(snap.equity, r.engine.equity()[2].equity);
}

TEST(Replay, OrderWaitsForOwnInstrumentBar) {
  // У бумаги B на шаге 3 бара нет: заявка, поданная на шаге 2, ждёт шага 4.
  const ReplayResult r = replay({place(0, 2, market(Side::Buy, 5, 1))}, 5);
  ASSERT_EQ(r.engine.fills().size(), 1u);
  EXPECT_EQ(r.engine.fills()[0].step, 4);
}

TEST(Replay, EventsFromStep) {
  const ReplayResult r = replay({place(0, 1, market(Side::Buy, 10))}, 5, 3);
  ASSERT_EQ(r.events.size(), 3u);
  EXPECT_EQ(r.events.front().step, 3);
  EXPECT_EQ(r.events.back().step, 5);
  // События шага 2 содержат исполнение рыночной заявки, поданной на шаге 1.
  const ReplayResult all = replay({place(0, 1, market(Side::Buy, 10))}, 5, 0);
  ASSERT_EQ(all.events.size(), 6u);
  EXPECT_EQ(all.events[2].fills.size(), 1u);
  EXPECT_EQ(all.events[2].changedOrders.size(), 1u);
  EXPECT_EQ(all.events[2].portfolio.lots[0], 10);
  EXPECT_EQ(all.events[1].changedOrders.size(), 1u); // подача на шаге 1
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
  SessionEngine engine(fixtureSpec(), fixtureTimeline());
  EXPECT_THROW(engine.apply(place(0, 1, market(Side::Buy, 1))),
               std::invalid_argument);
  engine.advance();
  EXPECT_THROW(engine.apply(place(0, 0, market(Side::Buy, 1))),
               std::invalid_argument);
  EXPECT_NO_THROW(engine.apply(place(0, 1, market(Side::Buy, 1))));
  EXPECT_THROW(engine.apply(place(0, 1, market(Side::Buy, 1))),
               std::invalid_argument); // seq не растёт
}

TEST(SessionEngine, RejectsBadSpec) {
  SessionSpec spec = fixtureSpec();
  spec.initialCash = D("0");
  EXPECT_THROW(SessionEngine(spec, fixtureTimeline()), std::invalid_argument);
  spec = fixtureSpec();
  spec.instruments.pop_back();
  EXPECT_THROW(SessionEngine(spec, fixtureTimeline()), std::invalid_argument);
}

TEST(SessionEngine, CopyIsSnapshot) {
  SessionEngine engine(fixtureSpec(), fixtureTimeline());
  engine.apply(place(0, 0, market(Side::Buy, 10)));
  const SessionEngine snapshot = engine;
  engine.advance();
  engine.advance();
  EXPECT_EQ(snapshot.currentStep(), 0);
  EXPECT_TRUE(snapshot.fills().empty());
  EXPECT_EQ(snapshot.equity().size(), 1u);
  EXPECT_EQ(engine.fills().size(), 1u);
  SessionEngine fork = snapshot; // из снимка можно идти дальше независимо
  fork.advance();
  EXPECT_EQ(fork.fills().size(), 1u);
  EXPECT_EQ(snapshot.fills().size(), 0u);
}

TEST(SessionEngine, AdvanceAfterStopThrows) {
  const ReplayResult r = replay({}, 9);
  SessionEngine engine = r.engine;
  EXPECT_THROW(engine.advance(), std::logic_error);
}

TEST(Replay, NaiveExecutesAtDecisionClose) {
  const ReplayResult r =
      replay({place(0, 2, market(Side::Buy, 10))}, 3, 0, true);
  ASSERT_EQ(r.engine.fills().size(), 1u);
  const Fill &f = r.engine.fills()[0];
  EXPECT_EQ(f.step, 2); // без задержки (ФТ-24)
  EXPECT_EQ(f.price, D("103")); // close бара решения, без издержек
  EXPECT_TRUE(f.commission.isZero());
  EXPECT_EQ(r.engine.positionValues()[2][0], D("10300"));
  EXPECT_EQ(r.engine.equity()[2].equity, D("1000000"));
}

TEST(Replay, NaiveSessionForcesZeroCosts) {
  // Издержки из spec в наивной сессии игнорируются: даже processStep исполняет
  // без комиссии, спреда и сбора.
  SessionSpec spec = fixtureSpec(true);
  spec.costs = CostConfig::defaults();
  const ReplayResult r = core::replaySession(
      spec, fixtureTimeline(),
      std::vector<JournalEntry>{place(0, 1, market(Side::Buy, 10))}, 3, 0);
  ASSERT_EQ(r.engine.fills().size(), 1u);
  EXPECT_TRUE(r.engine.fills()[0].commission.isZero());
  EXPECT_TRUE(r.engine.fills()[0].fee.isZero());
  EXPECT_TRUE(r.engine.fills()[0].spreadCost.isZero());
  EXPECT_EQ(r.engine.fills()[0].price, r.engine.fills()[0].basePrice);
}

TEST(Replay, NaiveCashCheckUsesZeroCosts) {
  // 1 лот A на шаге 2 стоит ровно 103 * 10 = 1030; без издержек этих денег
  // хватает.
  SessionSpec spec = fixtureSpec(true);
  spec.costs = CostConfig::defaults();
  spec.initialCash = D("1030");
  const ReplayResult r = core::replaySession(
      spec, fixtureTimeline(),
      std::vector<JournalEntry>{place(0, 2, market(Side::Buy, 1))}, 2, 0);
  EXPECT_TRUE(r.commandResults[0].accepted);
  ASSERT_EQ(r.engine.fills().size(), 1u);
  EXPECT_EQ(r.engine.fills()[0].value, D("1030"));
  EXPECT_EQ(r.engine.portfolio().cash(), D("0"));
}

TEST(Replay, NaiveStepClosedAfterSettle) {
  ReplayResult r = replay({place(0, 2, market(Side::Buy, 10))}, 2, 0, true);
  EXPECT_THROW(r.engine.apply(place(1, 2, market(Side::Sell, 10))),
               std::logic_error);
  // Следующий шаг открыт.
  r.engine.advance();
  EXPECT_NO_THROW(r.engine.apply(place(1, 3, market(Side::Sell, 10))));
}

TEST(Replay, NaiveConditionalOrderFillsWithZeroCosts) {
  // Лимитная покупка A по 101, поданная на шаге 1, исполняется на шаге 2 (open
  // = 101) через processStep - без издержек.
  SessionSpec spec = fixtureSpec(true);
  spec.costs = CostConfig::defaults();
  const ReplayResult r = core::replaySession(
      spec, fixtureTimeline(),
      std::vector<JournalEntry>{place(0, 1, limit(Side::Buy, 10, "101"))}, 3,
      0);
  ASSERT_EQ(r.engine.fills().size(), 1u);
  const Fill &f = r.engine.fills()[0];
  EXPECT_EQ(f.step, 2);
  EXPECT_EQ(f.price, D("101"));
  EXPECT_EQ(f.value, D("10100"));
  EXPECT_TRUE(f.commission.isZero());
  EXPECT_TRUE(f.fee.isZero());
  EXPECT_TRUE(f.spreadCost.isZero());
  EXPECT_TRUE(f.slippageCost.isZero());
  // Для сравнения: честная сессия берёт с той же заявки комиссию.
  const ReplayResult honest =
      replay({place(0, 1, limit(Side::Buy, 10, "101"))}, 3);
  ASSERT_EQ(honest.engine.fills().size(), 1u);
  EXPECT_FALSE(honest.engine.fills()[0].commission.isZero());
}

TEST(Replay, CancelOcoLegCancelsPair) {
  JournalEntry pair;
  pair.seq = 0;
  pair.step = 1;
  pair.command =
      PlaceCmd{limit(Side::Buy, 5, "90"), limit(Side::Buy, 3, "0.01", 1)};
  const ReplayResult r = replay({pair, cancel(1, 2, 1)}, 3);
  EXPECT_TRUE(r.commandResults[1].accepted);
  EXPECT_EQ(orderById(r.engine, 0).status, OrderStatus::Cancelled);
  EXPECT_EQ(orderById(r.engine, 0).reason, Reason::CancelledByUser);
  EXPECT_EQ(orderById(r.engine, 1).status, OrderStatus::Cancelled);
  EXPECT_EQ(orderById(r.engine, 1).closedStep, 2);
  // Повторная отмена - OrderNotActive.
  const ReplayResult again =
      replay({pair, cancel(1, 2, 1), cancel(2, 2, 0)}, 3);
  EXPECT_FALSE(again.commandResults[2].accepted);
  EXPECT_EQ(again.commandResults[2].reason, Reason::OrderNotActive);
}

TEST(Replay, NaiveSettlesAfterAllEntriesOfStep) {
  // Покупка и продажа на одном шаге: продажа проверяется до исполнения покупки
  // шага.
  const ReplayResult r = replay(
      {place(0, 2, market(Side::Buy, 10)), place(1, 2, market(Side::Sell, 10))},
      3, 0, true);
  EXPECT_TRUE(r.commandResults[0].accepted);
  EXPECT_FALSE(r.commandResults[1].accepted);
  EXPECT_EQ(r.commandResults[1].reason, Reason::InsufficientPosition);
  ASSERT_EQ(r.engine.fills().size(), 1u);
  EXPECT_EQ(r.events[2].fills.size(), 1u); // исполнение попало в события шага 2
}

// Одна бумага, лот 1, шаг цены 0.01, 4 дня; close по списку (open = close
// предыдущего бара).
std::shared_ptr<const Timeline>
singleTimeline(const std::vector<const char *> &closes) {
  const std::int64_t dayMs = 86'400'000;
  const std::int64_t start = 1'704'747'600'000; // 2024-01-09 00:00 МСК
  std::vector<Bar> bars;
  std::vector<TradingDay> calendar;
  Decimal prev = D(closes[0]);
  for (std::size_t i = 0; i < closes.size(); ++i) {
    Bar b;
    b.tsOpenMs = start + static_cast<std::int64_t>(i) * dayMs;
    b.availableAtMs = b.tsOpenMs + dayMs;
    b.open = prev;
    b.close = D(closes[i]);
    b.high = std::max(b.open, b.close);
    b.low = std::min(b.open, b.close);
    b.volume = 1000;
    prev = b.close;
    bars.push_back(b);
    calendar.push_back(
        TradingDay{static_cast<std::int32_t>(20240109 + i), true});
  }
  const std::vector<Instrument> instruments = {
      Instrument{"A", "SOLO", 1, D("0.001")}};
  return std::make_shared<const Timeline>(
      Timeline::build(instruments, {bars}, calendar));
}

TEST(Replay, BankruptStopsSession) {
  SessionSpec spec;
  spec.instruments = {Instrument{"A", "SOLO", 1, D("0.001")}};
  spec.costs = CostConfig::zero();
  spec.initialCash = D("0.99");
  const auto timeline =
      singleTimeline({"0.99", "0.99", "0.001", "0.001", "0.001"});
  const std::vector<JournalEntry> journal = {place(0, 0, market(Side::Buy, 1))};
  // Покупка на весь капитал, затем цена падает так, что позиция стоит 0,00 ->
  // капитал 0.
  const ReplayResult r = core::replaySession(spec, timeline, journal, 4, 0);
  EXPECT_EQ(r.status, SessionStatus::Bankrupt);
  EXPECT_EQ(r.lastStep, 2);
  EXPECT_EQ(r.engine.equity().size(), 3u);
  EXPECT_EQ(r.events.size(), 3u);
  EXPECT_EQ(r.engine.equity()[1].equity, D("0.99"));
  EXPECT_TRUE(r.engine.equity()[2].equity.isZero());
  // Прогон до шага 1 ещё идёт.
  EXPECT_EQ(core::replaySession(spec, timeline, journal, 1, 0).status,
            SessionStatus::Running);
  // Запись после банкротства - отказ SessionNotActive, журнал остаётся
  // проигрываемым.
  const ReplayResult late = core::replaySession(
      spec, timeline,
      std::vector<JournalEntry>{place(0, 0, market(Side::Buy, 1)),
                                place(1, 3, market(Side::Buy, 1))},
      4, 0);
  ASSERT_EQ(late.commandResults.size(), 2u);
  EXPECT_FALSE(late.commandResults[1].accepted);
  EXPECT_EQ(late.commandResults[1].reason, Reason::SessionNotActive);
  EXPECT_EQ(late.lastStep, 2);
  EXPECT_EQ(late.engine.orders().back().status, OrderStatus::Rejected);
  EXPECT_EQ(late.events.back().changedOrders.back().id,
            2); // отказ виден в событиях последнего шага
}

} // namespace
