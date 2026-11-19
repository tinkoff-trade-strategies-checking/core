#include <gtest/gtest.h>

#include <optional>
#include <variant>
#include <vector>

#include "convert/state.hpp"
#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "support/fixture1.hpp"

// Запись журнала ядра -> proto -> ядро без потерь (ReplaySession).

namespace {

namespace pb = pac::simulation::v1;

struct JournalWorld {
  core::SessionSpec Spec;
  core::Timeline Timeline;
};

JournalWorld journalWorld() {
  auto F = core::testing::fixture1();
  core::SessionSpec Spec;
  Spec.Instruments = F.Instruments;
  Spec.Costs = core::CostConfig::defaults();
  Spec.InitialCash = core::Decimal::parse("1000000");
  return JournalWorld{Spec, core::Timeline::load(
                                F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                                core::Timeframe::D1, 20240109, 20240122)};
}

bool sameSpec(const core::OrderSpec &A, const core::OrderSpec &B) {
  return A.Instrument == B.Instrument && A.OrderSide == B.OrderSide &&
         A.Type == B.Type && A.Lots == B.Lots && A.LimitPrice == B.LimitPrice &&
         A.StopPrice == B.StopPrice && A.TrailingPercent == B.TrailingPercent &&
         A.Tif == B.Tif && A.IsBlindPrice == B.IsBlindPrice;
}

core::JournalEntry roundTrip(const core::JournalEntry &E,
                             const JournalWorld &S) {
  pb::JournalEntry Proto;
  worker::toProto(E, S.Timeline, &Proto);
  return worker::fromProto(Proto, S.Spec);
}

} // namespace

TEST(ConvertJournal, PlaceWithOcoAndRule) {
  const JournalWorld S = journalWorld();
  core::OrderSpec Take;
  Take.Instrument = 1;
  Take.OrderSide = core::Side::Sell;
  Take.Type = core::OrderType::TakeProfit;
  Take.Lots = 2;
  Take.LimitPrice = core::Decimal::parse("0.025");
  core::OrderSpec Stop = Take;
  Stop.Type = core::OrderType::Stop;
  Stop.LimitPrice.reset();
  Stop.StopPrice = core::Decimal::parse("0.015");
  Stop.Tif = core::TimeInForce::Day;
  core::JournalEntry E;
  E.Seq = 7;
  E.Step = 3;
  E.Command = core::PlaceCmd{Take, Stop};

  const core::JournalEntry Back = roundTrip(E, S);
  EXPECT_EQ(Back.Seq, 7);
  EXPECT_EQ(Back.Step, 3);
  const auto &Place = std::get<core::PlaceCmd>(Back.Command);
  EXPECT_TRUE(sameSpec(Place.Order, Take));
  ASSERT_TRUE(Place.OcoWith);
  EXPECT_TRUE(sameSpec(*Place.OcoWith, Stop));
}

TEST(ConvertJournal, CancelKeepsReason) {
  const JournalWorld S = journalWorld();
  core::JournalEntry E;
  E.Seq = 1;
  E.Step = 4;
  E.Command = core::CancelCmd{5, core::Reason::Replaced};
  const core::JournalEntry Back = roundTrip(E, S);
  const auto &Cancel = std::get<core::CancelCmd>(Back.Command);
  EXPECT_EQ(Cancel.OrderId, 5);
  EXPECT_EQ(Cancel.ReasonCode, core::Reason::Replaced);

  // Отмена человека без причины - CANCELLED_BY_USER, как раньше.
  pb::JournalEntry Human;
  Human.set_seq(2);
  Human.set_step(4);
  Human.mutable_cancel()->set_order_id(5);
  EXPECT_EQ(std::get<core::CancelCmd>(worker::fromProto(Human, S.Spec).Command)
                .ReasonCode,
            core::Reason::CancelledByUser);
}

TEST(ConvertJournal, ReplaceWithTrailing) {
  const JournalWorld S = journalWorld();
  core::OrderSpec Trailing;
  Trailing.Instrument = 0;
  Trailing.OrderSide = core::Side::Sell;
  Trailing.Type = core::OrderType::TrailingStop;
  Trailing.Lots = 3;
  Trailing.TrailingPercent = core::Decimal::parse("4");
  core::JournalEntry E;
  E.Seq = 3;
  E.Step = 5;
  E.Command = core::ReplaceCmd{2, core::PlaceCmd{Trailing, std::nullopt}};
  const core::JournalEntry Back = roundTrip(E, S);
  const auto &Replace = std::get<core::ReplaceCmd>(Back.Command);
  EXPECT_EQ(Replace.OrderId, 2);
  EXPECT_TRUE(sameSpec(Replace.Replacement.Order, Trailing));
  EXPECT_FALSE(Replace.Replacement.OcoWith);
}

TEST(ConvertJournal, EveryReasonMapsBack) {
  for (int I = static_cast<int>(core::Reason::None);
       I <= static_cast<int>(core::Reason::DayExpired); ++I) {
    const auto Reason = static_cast<core::Reason>(I);
    EXPECT_EQ(worker::reasonFromProto(worker::toProto(Reason)), Reason) << I;
  }
}
