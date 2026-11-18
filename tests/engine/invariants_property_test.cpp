// Тесты на свойства (property-based) для инвариантов ядра:
// INV-1 (Честность времени / нет заглядывания в будущее)
// INV-2 (Тайминг исполнения заявок)
// INV-4 (Детерминизм сессии)
// INV-8 (Воспроизводимость состояния из журнала)
// INV-6 (Сохранение денег через движок исполнения)
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

  // Номер шага бара: у FIXA есть бар на каждом шаге, у FIXB бара шага 3 нет,
  // поэтому шаг бара FIXB определяется по времени открытия, а не по индексу.
  const auto StepOf = [&BarsA](const Bar &B) {
    const auto It =
        std::find_if(BarsA.begin(), BarsA.end(),
                     [&B](const Bar &A) { return A.TsOpenMs == B.TsOpenMs; });
    return static_cast<std::int64_t>(It - BarsA.begin());
  };
  std::vector<std::int64_t> StepsB;
  for (const Bar &B : BarsB)
    StepsB.push_back(StepOf(B));
  const auto Mutate = [&](std::vector<Bar> &Bars,
                          const std::vector<std::int64_t> &Steps, int Scale) {
    for (std::size_t I = 0; I < Bars.size(); ++I) {
      if (Steps[I] <= MutateAfterStep)
        continue;
      Bar &B = Bars[I];
      B.Open = B.Open.mul(Multiplier, Scale, RoundingMode::HalfUp);
      B.High = B.High.mul(Multiplier, Scale, RoundingMode::HalfUp);
      B.Low = B.Low.mul(Multiplier, Scale, RoundingMode::HalfUp);
      B.Close = B.Close.mul(Multiplier, Scale, RoundingMode::HalfUp);
    }
  };
  std::vector<std::int64_t> StepsA;
  for (std::size_t I = 0; I < BarsA.size(); ++I)
    StepsA.push_back(static_cast<std::int64_t>(I));
  Mutate(BarsA, StepsA, 2);
  Mutate(BarsB, StepsB, 6);

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

// Цена бумаги около цены фикстуры: FIXA - 90…115, FIXB - 0.0190…0.0210 с
// шагом 0.0005 (кратно шагу цены). Часть цен окажется не с той стороны от
// close - такая заявка отклоняется валидацией, это тоже проверяемый путь.
Decimal genPrice(int Inst) {
  if (Inst == 0)
    return Decimal::fromUnits(*rc::gen::inRange<std::int64_t>(90, 116));
  const auto Ticks = *rc::gen::inRange<std::int64_t>(38, 43);
  return Decimal::parse("0.0005").times(Ticks);
}

// Заявка любого вида: рыночная, лимитная, стоп, стоп-лимит, тейк-профит,
// трейлинг; срок «день» или GTC.
OrderSpec genOrderSpec(int Inst, Side OrderSide) {
  OrderSpec S;
  S.Instrument = Inst;
  S.OrderSide = OrderSide;
  S.Lots = *rc::gen::inRange<std::int64_t>(1, 10);
  S.Tif = *rc::gen::element(TimeInForce::Gtc, TimeInForce::Day);
  switch (*rc::gen::inRange(0, 6)) {
  case 0:
    S.Type = OrderType::Market;
    break;
  case 1:
    S.Type = OrderType::Limit;
    S.LimitPrice = genPrice(Inst);
    break;
  case 2:
    S.Type = OrderType::Stop;
    S.StopPrice = genPrice(Inst);
    break;
  case 3:
    S.Type = OrderType::StopLimit;
    S.StopPrice = genPrice(Inst);
    S.LimitPrice = genPrice(Inst);
    break;
  case 4:
    S.Type = OrderType::TakeProfit;
    S.OrderSide = Side::Sell;
    S.LimitPrice = genPrice(Inst);
    break;
  default:
    S.Type = OrderType::TrailingStop;
    S.OrderSide = Side::Sell;
    S.TrailingPercent =
        Decimal::fromUnits(*rc::gen::inRange<std::int64_t>(1, 20));
    break;
  }
  return S;
}

// Пара OCO «стоп + тейк» (продажа одной бумаги).
PlaceCmd genOcoPlace(int Inst) {
  OrderSpec StopLeg;
  StopLeg.Instrument = Inst;
  StopLeg.OrderSide = Side::Sell;
  StopLeg.Lots = *rc::gen::inRange<std::int64_t>(1, 10);
  StopLeg.Tif = *rc::gen::element(TimeInForce::Gtc, TimeInForce::Day);
  StopLeg.Type = *rc::gen::element(OrderType::Stop, OrderType::StopLimit);
  StopLeg.StopPrice = genPrice(Inst);
  if (StopLeg.Type == OrderType::StopLimit)
    StopLeg.LimitPrice = genPrice(Inst);
  OrderSpec Take = StopLeg;
  Take.Type = OrderType::TakeProfit;
  Take.StopPrice.reset();
  Take.LimitPrice = genPrice(Inst);
  return PlaceCmd{std::move(StopLeg), std::move(Take)};
}

PlaceCmd genPlace() {
  const int Inst = *rc::gen::inRange(0, 2);
  if (*rc::gen::inRange(0, 6) == 0)
    return genOcoPlace(Inst);
  const Side OrderSide = *rc::gen::element(Side::Buy, Side::Sell);
  return PlaceCmd{genOrderSpec(Inst, OrderSide), std::nullopt};
}

JournalEntry genJournalEntry(std::int64_t Seq, std::int64_t Step,
                             const std::vector<std::int64_t> &ActiveOrderIds) {
  JournalEntry E;
  E.Seq = Seq;
  E.Step = Step;
  const auto Kind = *rc::gen::inRange(0, 4); // 0 - отмена, 1 - замена
  if (!ActiveOrderIds.empty() && Kind == 0) {
    const auto Idx = *rc::gen::inRange<std::size_t>(0, ActiveOrderIds.size());
    E.Command = CancelCmd{ActiveOrderIds[Idx]};
  } else if (!ActiveOrderIds.empty() && Kind == 1) {
    const auto Idx = *rc::gen::inRange<std::size_t>(0, ActiveOrderIds.size());
    E.Command = ReplaceCmd{ActiveOrderIds[Idx], genPlace()};
  } else {
    E.Command = genPlace();
  }
  return E;
}

std::vector<JournalEntry> genRandomJournal(std::int64_t MaxStep = 7) {
  const auto Count = *rc::gen::inRange(1, 15);
  std::vector<JournalEntry> Journal;
  Journal.reserve(Count);

  std::vector<std::int64_t> Steps;
  Steps.reserve(static_cast<std::size_t>(Count));
  for (int I = 0; I < Count; ++I)
    Steps.push_back(*rc::gen::inRange<std::int64_t>(0, MaxStep + 1));
  std::sort(Steps.begin(), Steps.end());

  std::vector<std::int64_t> PlacedOrderIds;
  for (int I = 0; I < Count; ++I) {
    JournalEntry E =
        genJournalEntry(I, Steps[static_cast<std::size_t>(I)], PlacedOrderIds);
    // id заявки = seq * 2 + нога (journal.hpp)
    const auto *Place = std::get_if<PlaceCmd>(&E.Command);
    const auto *Replace = std::get_if<ReplaceCmd>(&E.Command);
    if (Replace != nullptr)
      Place = &Replace->Replacement;
    if (Place != nullptr) {
      PlacedOrderIds.push_back(I * 2);
      if (Place->OcoWith)
        PlacedOrderIds.push_back(I * 2 + 1);
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

// INV-6: деньги сходятся до копейки: cash = начальный - покупки + продажи -
// комиссии и сборы по всем сделкам, которые породил движок при проигрывании
// случайного журнала (а не по прямым вызовам Portfolio::applyFill).
RC_GTEST_PROP(InvariantsProperty, CashConservationThroughEngine, ()) {
  const auto UpTo = *rc::gen::inRange<std::int64_t>(3, 10);
  const auto Journal = genRandomJournal(UpTo);
  const SessionSpec Spec = fixtureSpec();

  const ReplayResult R =
      replaySession(Spec, fixtureTimeline(), Journal, UpTo, 0);

  Decimal Expected = Spec.InitialCash;
  for (const Fill &F : R.Engine.fills()) {
    if (F.OrderSide == Side::Buy)
      Expected = Expected - F.Value;
    else
      Expected = Expected + F.Value;
    Expected = Expected - F.Commission - F.Fee;
  }
  RC_ASSERT(R.Engine.portfolio().cash() == Expected);
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
