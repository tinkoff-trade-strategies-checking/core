// Тесты на свойства (property-based) для инвариантов ядра:
// INV-1 (Честность времени / нет заглядывания в будущее)
// INV-2 (Тайминг исполнения заявок)
// INV-4 (Детерминизм сессии)
// INV-8 (Воспроизводимость состояния из журнала)
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "core/engine/digest.hpp"
#include "core/engine/session_engine.hpp"
#include "core/market/bar.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/money/rounding.hpp"
#include "support/fixture1.hpp"

namespace core {
namespace {

std::shared_ptr<const Timeline> fixtureTimeline() {
  auto F = testing::fixture1();
  return std::make_shared<const Timeline>(
      Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                     Timeframe::D1, 20240109, 20240122));
}

std::shared_ptr<const Timeline> mutatedTimeline(std::int64_t MutateAfterStep,
                                                Decimal Multiplier) {
  auto F = testing::fixture1();
  BarRequest ReqA{F.SnapshotId, "FIXA", 86400, 0, 2'000'000'000'000LL};
  BarRequest ReqB{F.SnapshotId, "FIXB", 86400, 0, 2'000'000'000'000LL};

  auto BarsA = F.Bars.load(ReqA);
  auto BarsB = F.Bars.load(ReqB);

  for (std::size_t I = 0; I < BarsA.size(); ++I) {
    if (static_cast<std::int64_t>(I) > MutateAfterStep) {
      BarsA[I].Open = BarsA[I].Open.mul(Multiplier, 2, RoundingMode::HalfUp);
      BarsA[I].High = BarsA[I].High.mul(Multiplier, 2, RoundingMode::HalfUp);
      BarsA[I].Low = BarsA[I].Low.mul(Multiplier, 2, RoundingMode::HalfUp);
      BarsA[I].Close = BarsA[I].Close.mul(Multiplier, 2, RoundingMode::HalfUp);
    }
  }

  for (std::size_t I = 0; I < BarsB.size(); ++I) {
    if (static_cast<std::int64_t>(I) > MutateAfterStep) {
      BarsB[I].Open = BarsB[I].Open.mul(Multiplier, 6, RoundingMode::HalfUp);
      BarsB[I].High = BarsB[I].High.mul(Multiplier, 6, RoundingMode::HalfUp);
      BarsB[I].Low = BarsB[I].Low.mul(Multiplier, 6, RoundingMode::HalfUp);
      BarsB[I].Close = BarsB[I].Close.mul(Multiplier, 6, RoundingMode::HalfUp);
    }
  }

  F.Bars.addSeries(F.SnapshotId, "FIXA", 86400, std::move(BarsA));
  F.Bars.addSeries(F.SnapshotId, "FIXB", 86400, std::move(BarsB));

  return std::make_shared<const Timeline>(
      Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                     Timeframe::D1, 20240109, 20240122));
}

SessionSpec fixtureSpec() {
  SessionSpec Spec;
  Spec.Instruments = testing::fixture1().Instruments;
  Spec.Costs = CostConfig::defaults();
  Spec.InitialCash = Decimal::parse("1000000");
  return Spec;
}

JournalEntry genJournalEntry(std::int64_t Seq, std::int64_t Step,
                             const std::vector<std::int64_t> &ActiveOrderIds) {
  const bool CanCancel = !ActiveOrderIds.empty() && *rc::gen::arbitrary<bool>();
  if (CanCancel) {
    const auto Idx = *rc::gen::inRange<std::size_t>(0, ActiveOrderIds.size());
    JournalEntry E;
    E.Seq = Seq;
    E.Step = Step;
    E.Command = CancelCmd{ActiveOrderIds[Idx]};
    return E;
  }

  const int Inst = *rc::gen::inRange(0, 2);
  const Side OrderSide = *rc::gen::element(Side::Buy, Side::Sell);
  const bool IsLimit = *rc::gen::arbitrary<bool>();

  OrderSpec S;
  S.Instrument = Inst;
  S.OrderSide = OrderSide;
  S.Lots = *rc::gen::inRange<std::int64_t>(1, 10);
  if (IsLimit) {
    S.Type = OrderType::Limit;
    S.Tif = TimeInForce::Gtc;
    if (Inst == 0) {
      const auto P = *rc::gen::inRange<std::int64_t>(95, 110);
      S.LimitPrice = Decimal::fromUnits(P);
    } else {
      S.LimitPrice = Decimal::parse("0.0200");
    }
  } else {
    S.Type = OrderType::Market;
  }

  JournalEntry E;
  E.Seq = Seq;
  E.Step = Step;
  E.Command = PlaceCmd{std::move(S), std::nullopt};
  return E;
}

std::vector<JournalEntry> genRandomJournal(std::int64_t MaxStep = 7) {
  const auto Count = *rc::gen::inRange(1, 15);
  std::vector<JournalEntry> Journal;
  Journal.reserve(Count);

  std::vector<std::int64_t> PlacedOrderIds;
  std::int64_t CurrentStep = 0;

  for (int I = 0; I < Count; ++I) {
    CurrentStep = *rc::gen::inRange(CurrentStep, MaxStep + 1);
    JournalEntry E = genJournalEntry(I, CurrentStep, PlacedOrderIds);
    if (std::holds_alternative<PlaceCmd>(E.Command)) {
      PlacedOrderIds.push_back(I * 2);
    }
    Journal.push_back(std::move(E));
  }
  return Journal;
}

void rcAssertSameState(const SessionEngine &A, const SessionEngine &B) {
  RC_ASSERT(digestOf(A) == digestOf(B));
  RC_ASSERT(A.portfolio().cash() == B.portfolio().cash());
  RC_ASSERT(A.status() == B.status());
  RC_ASSERT(A.currentStep() == B.currentStep());
  RC_ASSERT(A.fills().size() == B.fills().size());
  for (std::size_t I = 0; I < A.fills().size(); ++I) {
    const auto &Fa = A.fills()[I];
    const auto &Fb = B.fills()[I];
    RC_ASSERT(Fa.OrderId == Fb.OrderId);
    RC_ASSERT(Fa.Step == Fb.Step);
    RC_ASSERT(Fa.Instrument == Fb.Instrument);
    RC_ASSERT(Fa.OrderSide == Fb.OrderSide);
    RC_ASSERT(Fa.Lots == Fb.Lots);
    RC_ASSERT(Fa.Price == Fb.Price);
    RC_ASSERT(Fa.Value == Fb.Value);
    RC_ASSERT(Fa.Commission == Fb.Commission);
    RC_ASSERT(Fa.Fee == Fb.Fee);
  }
  RC_ASSERT(A.orders().size() == B.orders().size());
  for (std::size_t I = 0; I < A.orders().size(); ++I) {
    const auto &Oa = A.orders()[I];
    const auto &Ob = B.orders()[I];
    RC_ASSERT(Oa.Id == Ob.Id);
    RC_ASSERT(Oa.Status == Ob.Status);
    RC_ASSERT(Oa.PlacedStep == Ob.PlacedStep);
    RC_ASSERT(Oa.ClosedStep == Ob.ClosedStep);
    RC_ASSERT(Oa.FilledLots == Ob.FilledLots);
  }
  RC_ASSERT(A.equity().size() == B.equity().size());
  for (std::size_t I = 0; I < A.equity().size(); ++I) {
    RC_ASSERT(A.equity()[I].Step == B.equity()[I].Step);
    RC_ASSERT(A.equity()[I].Equity == B.equity()[I].Equity);
  }
}

// INV-4: Детерминизм сессии
RC_GTEST_PROP(InvariantsProperty, DeterministicReplay, ()) {
  const auto UpTo = *rc::gen::inRange<std::int64_t>(3, 10);
  const auto Journal = genRandomJournal(UpTo);

  const ReplayResult R1 =
      replaySession(fixtureSpec(), fixtureTimeline(), Journal, UpTo, 0);
  const ReplayResult R2 =
      replaySession(fixtureSpec(), fixtureTimeline(), Journal, UpTo, 0);

  RC_ASSERT(digestOf(R1.Engine) != 0);
  rcAssertSameState(R1.Engine, R2.Engine);
}

// INV-2: Тайминг заявок (исполнение не раньше open бара K+1)
RC_GTEST_PROP(InvariantsProperty, OrderExecutionTiming, ()) {
  const auto UpTo = *rc::gen::inRange<std::int64_t>(3, 10);
  const auto Journal = genRandomJournal(UpTo);

  const ReplayResult R =
      replaySession(fixtureSpec(), fixtureTimeline(), Journal, UpTo, 0);

  for (const Fill &F : R.Engine.fills()) {
    const auto &Orders = R.Engine.orders();
    const auto It = std::lower_bound(
        Orders.begin(), Orders.end(), F.OrderId,
        [](const Order &O, std::int64_t Id) { return O.Id < Id; });
    RC_ASSERT(It != Orders.end());
    RC_ASSERT(It->Id == F.OrderId);

    // Заявка с шага K не исполняется раньше open бара K+1
    RC_ASSERT(F.Step >= It->PlacedStep + 1);
    RC_ASSERT(F.Step <= R.LastStep);
  }
}

// INV-8: Воспроизводимость состояния из любой точки обрыва журнала
RC_GTEST_PROP(InvariantsProperty, ReplayResumeFromCheckpoint, ()) {
  const auto UpTo = *rc::gen::inRange<std::int64_t>(4, 10);
  const auto Journal = genRandomJournal(UpTo);

  const ReplayResult Full =
      replaySession(fixtureSpec(), fixtureTimeline(), Journal, UpTo, 0);

  const auto CheckpointStep = *rc::gen::inRange<std::int64_t>(0, UpTo);

  std::vector<JournalEntry> Prefix;
  for (const auto &E : Journal) {
    if (E.Step <= CheckpointStep) {
      Prefix.push_back(E);
    }
  }

  SessionEngine Resumed =
      replaySession(fixtureSpec(), fixtureTimeline(), Prefix, CheckpointStep, 0)
          .Engine;

  std::size_t Next = Prefix.size();
  for (std::int64_t S = CheckpointStep;; ++S) {
    while (Next < Journal.size() &&
           (Journal[Next].Step == S ||
            Resumed.status() != SessionStatus::Running)) {
      Resumed.apply(Journal[Next++]);
    }
    if (S == UpTo || Resumed.status() != SessionStatus::Running) {
      break;
    }
    Resumed.advance();
  }

  rcAssertSameState(Full.Engine, Resumed);
}

// INV-1: Честность времени / отсутствие заглядывания в будущее
RC_GTEST_PROP(InvariantsProperty, FutureBarsDoNotAffectPastSteps, ()) {
  const auto StepN = *rc::gen::inRange<std::int64_t>(1, 7);
  const auto Journal = genRandomJournal(StepN);

  const auto MultiplierFactor = *rc::gen::inRange<std::int64_t>(2, 10);
  const Decimal Multiplier = Decimal::fromUnits(MultiplierFactor);

  const auto BaseTL = fixtureTimeline();
  const auto MutTL = mutatedTimeline(StepN, Multiplier);

  const ReplayResult BaseRes =
      replaySession(fixtureSpec(), BaseTL, Journal, StepN, 0);
  const ReplayResult MutRes =
      replaySession(fixtureSpec(), MutTL, Journal, StepN, 0);

  // Подмена будущих баров не может изменить состояние движка на шаге N
  rcAssertSameState(BaseRes.Engine, MutRes.Engine);
}

// Мутационная проверка: подтверждаем, что мутированные бары на шагах > N
// реально изменяют состояние, если симуляция заходит на эти шаги.
TEST(InvariantsProperty, MutatedFutureDivergesOnLaterStep) {
  const std::int64_t StepN = 3;
  std::vector<JournalEntry> Journal;
  OrderSpec S;
  S.Instrument = 0;
  S.OrderSide = Side::Buy;
  S.Type = OrderType::Market;
  S.Lots = 10;
  JournalEntry E;
  E.Seq = 0;
  E.Step = 1;
  E.Command = PlaceCmd{std::move(S), std::nullopt};
  Journal.push_back(E);

  const auto BaseTL = fixtureTimeline();
  const auto MutTL = mutatedTimeline(StepN, Decimal::parse("3.5"));

  const ReplayResult BaseRes =
      replaySession(fixtureSpec(), BaseTL, Journal, 5, 0);
  const ReplayResult MutRes =
      replaySession(fixtureSpec(), MutTL, Journal, 5, 0);

  EXPECT_NE(digestOf(BaseRes.Engine), digestOf(MutRes.Engine));
  EXPECT_NE(BaseRes.Engine.equity().back().Equity,
            MutRes.Engine.equity().back().Equity);
}

} // namespace
} // namespace core
