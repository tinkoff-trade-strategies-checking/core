#include <gtest/gtest.h>

#include <optional>
#include <variant>
#include <vector>

#include "convert/state.hpp"
#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "support/fixture1.hpp"

// Запись журнала ядра -> proto -> ядро без потерь (RunResult.journal ->
// ReplaySession).

namespace {

namespace pb = pac::simulation::v1;

struct JournalWorld {
  core::SessionSpec spec;
  core::Timeline timeline;
};

JournalWorld journalWorld() {
  auto f = core::testing::fixture1();
  core::SessionSpec spec;
  spec.instruments = f.instruments;
  spec.costs = core::CostConfig::defaults();
  spec.initialCash = core::Decimal::parse("1000000");
  return JournalWorld{spec, core::Timeline::load(
                                f.bars, f.calendar, f.snapshotId, f.instruments,
                                core::Timeframe::D1, 20240109, 20240122)};
}

bool sameSpec(const core::OrderSpec &a, const core::OrderSpec &b) {
  return a.instrument == b.instrument && a.side == b.side && a.type == b.type &&
         a.lots == b.lots && a.limitPrice == b.limitPrice &&
         a.stopPrice == b.stopPrice && a.trailingPercent == b.trailingPercent &&
         a.tif == b.tif;
}

core::JournalEntry roundTrip(const core::JournalEntry &e,
                             const JournalWorld &s) {
  pb::JournalEntry proto;
  worker::toProto(e, s.timeline, &proto);
  return worker::fromProto(proto, s.spec);
}

} // namespace

TEST(ConvertJournal, PlaceWithOcoAndRule) {
  const JournalWorld s = journalWorld();
  core::OrderSpec take;
  take.instrument = 1;
  take.side = core::Side::Sell;
  take.type = core::OrderType::TakeProfit;
  take.lots = 2;
  take.limitPrice = core::Decimal::parse("0.025");
  core::OrderSpec stop = take;
  stop.type = core::OrderType::Stop;
  stop.limitPrice.reset();
  stop.stopPrice = core::Decimal::parse("0.015");
  stop.tif = core::TimeInForce::Day;
  core::JournalEntry e;
  e.seq = 7;
  e.step = 3;
  e.command = core::PlaceCmd{take, stop};
  e.ruleName = "защита";
  e.because = "тейк и стоп";

  const core::JournalEntry back = roundTrip(e, s);
  EXPECT_EQ(back.seq, 7);
  EXPECT_EQ(back.step, 3);
  EXPECT_EQ(back.ruleName, "защита");
  EXPECT_EQ(back.because, "тейк и стоп");
  const auto &place = std::get<core::PlaceCmd>(back.command);
  EXPECT_TRUE(sameSpec(place.order, take));
  ASSERT_TRUE(place.ocoWith);
  EXPECT_TRUE(sameSpec(*place.ocoWith, stop));
}

TEST(ConvertJournal, CancelKeepsReason) {
  const JournalWorld s = journalWorld();
  core::JournalEntry e;
  e.seq = 1;
  e.step = 4;
  e.command = core::CancelCmd{5, core::Reason::PositionClosed};
  e.ruleName = "s";
  e.because = "позиция закрыта";
  const core::JournalEntry back = roundTrip(e, s);
  const auto &cancel = std::get<core::CancelCmd>(back.command);
  EXPECT_EQ(cancel.orderId, 5);
  EXPECT_EQ(cancel.reason, core::Reason::PositionClosed);
  EXPECT_EQ(back.ruleName, "s");

  // Отмена человека без причины - CANCELLED_BY_USER, как раньше.
  pb::JournalEntry human;
  human.set_seq(2);
  human.set_step(4);
  human.mutable_cancel()->set_order_id(5);
  EXPECT_EQ(std::get<core::CancelCmd>(worker::fromProto(human, s.spec).command)
                .reason,
            core::Reason::CancelledByUser);
}

TEST(ConvertJournal, ReplaceWithTrailing) {
  const JournalWorld s = journalWorld();
  core::OrderSpec trailing;
  trailing.instrument = 0;
  trailing.side = core::Side::Sell;
  trailing.type = core::OrderType::TrailingStop;
  trailing.lots = 3;
  trailing.trailingPercent = core::Decimal::parse("4");
  core::JournalEntry e;
  e.seq = 3;
  e.step = 5;
  e.command = core::ReplaceCmd{2, core::PlaceCmd{trailing, std::nullopt}};
  e.ruleName = "r";
  const core::JournalEntry back = roundTrip(e, s);
  const auto &replace = std::get<core::ReplaceCmd>(back.command);
  EXPECT_EQ(replace.orderId, 2);
  EXPECT_TRUE(sameSpec(replace.replacement.order, trailing));
  EXPECT_FALSE(replace.replacement.ocoWith);
}

TEST(ConvertJournal, EveryReasonMapsBack) {
  for (int i = static_cast<int>(core::Reason::None);
       i <= static_cast<int>(core::Reason::DayExpired); ++i) {
    const auto reason = static_cast<core::Reason>(i);
    EXPECT_EQ(worker::reasonFromProto(worker::toProto(reason)), reason) << i;
  }
}
