#include "core/engine/session_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace core {

namespace {

bool isActive(const Order &O) {
  return O.Status == OrderStatus::Active ||
         O.Status == OrderStatus::PartiallyFilled;
}

} // namespace

SessionEngine::SessionEngine(SessionSpec InSpec,
                             std::shared_ptr<const Timeline> Tl)
    : Spec(std::move(InSpec)), TimelineRef(std::move(Tl)),
      Exec(Spec.Instruments, Spec.Costs),
      PortfolioState(Spec.InitialCash, Spec.Instruments.size()),
      BarsInPosition(Spec.Instruments.size(), std::nullopt),
      PositionReopenedOnBar(Spec.Instruments.size(), false) {
  if (!TimelineRef)
    throw std::invalid_argument("SessionEngine: нет шкалы времени");
  if (TimelineRef->totalSteps() < 1)
    throw std::invalid_argument("SessionEngine: шкала времени без шагов");
  if (Spec.Instruments != TimelineRef->instruments()) {
    throw std::invalid_argument(
        "SessionEngine: бумаги сессии не совпадают с бумагами шкалы времени");
  }
  if (Spec.InitialCash.sign() <= 0)
    throw std::invalid_argument(
        "SessionEngine: начальный капитал должен быть > 0");
  validateCostConfig(Spec.Costs);
  recordStepState();
  // Единственный шаг шкалы - он же последний: сессия сразу закончена.
  if (TimelineRef->totalSteps() == 1)
    Status = SessionStatus::FinishedEnd;
}

Validation
SessionEngine::validateLegs(const std::vector<const OrderSpec *> &Legs) const {
  if (Status != SessionStatus::Running)
    return Validation{false, Reason::SessionNotActive, ""};
  if (Legs.size() == 2) {
    Validation OcoVerdict = validateOcoPair(*Legs[0], *Legs[1]);
    if (!OcoVerdict.Ok)
      return OcoVerdict;
  }
  for (const OrderSpec *Leg : Legs) {
    Validation V = Exec.validate(*Leg, PortfolioState, *TimelineRef, Step);
    if (!V.Ok)
      return V;
  }
  return Validation{};
}

// Создаёт заявки команды: при отказе - со статусом Rejected и причиной (запись
// остаётся в журнале).
void SessionEngine::createOrders(const JournalEntry &Entry, const PlaceCmd &Cmd,
                                 const Validation &Verdict,
                                 CommandResult &Result) {
  std::vector<const OrderSpec *> Legs{&Cmd.Order};
  if (Cmd.OcoWith)
    Legs.push_back(&*Cmd.OcoWith);
  std::vector<std::int64_t> Ids;
  for (std::size_t I = 0; I < Legs.size(); ++I)
    Ids.push_back(Entry.Seq * 2 + static_cast<std::int64_t>(I));

  for (std::size_t I = 0; I < Legs.size(); ++I) {
    Order O;
    O.Id = Ids[I];
    O.Spec = *Legs[I];
    O.PlacedStep = Step;
    O.RuleName = Entry.RuleName;
    O.Because = Entry.Because;
    if (Verdict.Ok) {
      O.Status = OrderStatus::Active;
      if (Legs.size() == 2)
        O.OcoSiblingId = Ids[1 - I];
    } else {
      O.Status = OrderStatus::Rejected;
      O.ReasonCode = Verdict.ReasonCode;
      O.InvalidField = Verdict.Field;
      O.ClosedStep = Step;
    }
    Orders.push_back(std::move(O));
    Changed.push_back(Ids[I]);
    Result.OrderIds.push_back(Ids[I]);
  }
  Result.Accepted = Verdict.Ok;
  Result.ReasonCode = Verdict.ReasonCode;
  Result.Field = Verdict.Field;
}

Order *SessionEngine::findOrder(std::int64_t Id) {
  const auto It = std::lower_bound(
      Orders.begin(), Orders.end(), Id,
      [](const Order &O, std::int64_t Value) { return O.Id < Value; });
  return It != Orders.end() && It->Id == Id ? &*It : nullptr;
}

void SessionEngine::closeOrder(Order &Order, OrderStatus FinalStatus,
                               Reason Reason) {
  Order.Status = FinalStatus;
  Order.ReasonCode = Reason;
  Order.ClosedStep = Step;
  Changed.push_back(Order.Id);
}

void SessionEngine::cancelActive(Reason Reason) {
  for (Order &O : Orders) {
    if (isActive(O))
      closeOrder(O, OrderStatus::Cancelled, Reason);
  }
}

CommandResult SessionEngine::apply(const JournalEntry &Entry) {
  // Остановленная сессия принимает и записи с более поздним шагом: они
  // отклоняются с SessionNotActive, чтобы журнал с такими записями оставался
  // проигрываемым.
  const bool Stopped = Status != SessionStatus::Running;
  if (Entry.Step != Step && !(Stopped && Entry.Step > Step)) {
    throw std::invalid_argument(
        "SessionEngine::apply: шаг записи " + std::to_string(Entry.Step) +
        " не равен текущему шагу " + std::to_string(Step));
  }
  if (Entry.Seq < 0 || Entry.Seq <= LastSeq) {
    throw std::invalid_argument(
        "SessionEngine::apply: seq " + std::to_string(Entry.Seq) +
        " не больше предыдущего " + std::to_string(LastSeq));
  }
  LastSeq = Entry.Seq;

  CommandResult Result;
  Result.Seq = Entry.Seq;
  const bool Running = Status == SessionStatus::Running;

  std::visit(
      [&](const auto &Cmd) {
        using T = std::decay_t<decltype(Cmd)>;
        if constexpr (std::is_same_v<T, PlaceCmd>) {
          std::vector<const OrderSpec *> Legs{&Cmd.Order};
          if (Cmd.OcoWith)
            Legs.push_back(&*Cmd.OcoWith);
          createOrders(Entry, Cmd, validateLegs(Legs), Result);
        } else if constexpr (std::is_same_v<T, CancelCmd>) {
          if (!Running) {
            Result.ReasonCode = Reason::SessionNotActive;
          } else if (cancelOrder(Orders, Cmd.OrderId, Cmd.ReasonCode, Step,
                                 Changed)) { // у OCO - вместе со второй ногой
            Result.Accepted = true;
          } else {
            Result.ReasonCode = Reason::OrderNotActive;
          }
        } else {
          // Замена: сначала проверяется новая заявка; не прошла - запись
          // отклонена целиком, старая не тронута.
          Order *Old = findOrder(Cmd.OrderId);
          std::vector<const OrderSpec *> Legs{&Cmd.Replacement.Order};
          if (Cmd.Replacement.OcoWith)
            Legs.push_back(&*Cmd.Replacement.OcoWith);
          Validation Verdict = validateLegs(Legs);
          if (Verdict.Ok && (Old == nullptr || !isActive(*Old)))
            Verdict = Validation{false, Reason::OrderNotActive, ""};
          if (Verdict.Ok) {
            cancelOrder(Orders, Old->Id, Reason::Replaced, Step,
                        Changed); // у OCO заменяется вся пара
          }
          createOrders(Entry, Cmd.Replacement, Verdict, Result);
        }
      },
      Entry.Command);

  return Result;
}

void SessionEngine::advance() {
  if (Status != SessionStatus::Running)
    throw std::logic_error("SessionEngine::advance: сессия остановлена");
  const std::size_t InstrumentCount = Spec.Instruments.size();
  std::vector<std::int64_t> PosBefore(InstrumentCount);
  for (std::size_t I = 0; I < InstrumentCount; ++I)
    PosBefore[I] = PortfolioState.lots(static_cast<int>(I));
  PositionReopenedOnBar.assign(InstrumentCount, false);

  const std::int64_t Next = Step + 1;
  Step = Next;
  FillsBegin = Fills.size();
  Changed.clear();

  // Дневные заявки снимает processStep (задача 14), движок своего снятия не
  // делает.
  Exec.processStep(Next, *TimelineRef, Orders, PortfolioState, Fills, Changed);
  checkIntegrity(FillsBegin);
  updatePositionBars(PosBefore);
  recordStepState();

  const bool Bankrupt = Equity.back().Equity.sign() <= 0;
  const bool LastStep = Next == TimelineRef->totalSteps() - 1;
  if (Bankrupt) {
    Status = SessionStatus::Bankrupt;
  } else if (LastStep) {
    Status = SessionStatus::FinishedEnd;
  }
  // Остановленная сессия не оставляет активных заявок.
  if (Status != SessionStatus::Running)
    cancelActive(Reason::SessionEnd);
}

// Состояние шага: переоценка по последним close, точка кривой стоимости и
// стоимости позиций.
void SessionEngine::recordStepState() {
  const std::size_t N = Spec.Instruments.size();
  std::vector<std::optional<Decimal>> LastClose(N);
  std::vector<std::int64_t> LotSizes(N);
  std::vector<Decimal> Values(N);
  for (std::size_t I = 0; I < N; ++I) {
    LotSizes[I] = Spec.Instruments[I].LotSize;
    const StepBar *Bar = TimelineRef->lastBarOf(static_cast<int>(I), Step);
    if (Bar != nullptr)
      LastClose[I] = Bar->Close;
    const std::int64_t Lots = PortfolioState.lots(static_cast<int>(I));
    if (Lots > 0 && Bar != nullptr)
      Values[I] = Portfolio::positionValue(Lots, LotSizes[I], Bar->Close);
  }
  EquityPoint Point{Step, PortfolioState.equity(LastClose, LotSizes)};
  Equity.push_back(Point);
  PositionValues.push_back(std::move(Values));
}

// INV-2: исполнения шага относятся к текущему шагу, известной заявке и идут
// строго после шага её подачи. Баров из будущего (INV-1) здесь нет:
// Timeline::barsAt(step) по построению отдаёт только бары шага step.
void SessionEngine::checkIntegrity(std::size_t FromFill) const {
  for (std::size_t I = FromFill; I < Fills.size(); ++I) {
    const Fill &F = Fills[I];
    if (F.Step != Step)
      throw IntegrityViolation("исполнение не на текущем шаге");
    const auto It = std::lower_bound(
        Orders.begin(), Orders.end(), F.OrderId,
        [](const Order &O, std::int64_t Value) { return O.Id < Value; });
    if (It == Orders.end() || It->Id != F.OrderId)
      throw IntegrityViolation("исполнение неизвестной заявки");
    if (F.Step <= It->PlacedStep) {
      throw IntegrityViolation("исполнение не позже шага подачи заявки");
    }
  }
}

void SessionEngine::updatePositionBars(
    const std::vector<std::int64_t> &PosBefore) {
  const std::size_t InstrumentCount = Spec.Instruments.size();
  for (std::size_t I = 0; I < InstrumentCount; ++I) {
    const int Instr = static_cast<int>(I);
    if (TimelineRef->barOf(Instr, Step) == nullptr) {
      PositionReopenedOnBar[I] = false;
      continue;
    }
    if (PortfolioState.lots(Instr) == 0) {
      BarsInPosition[I] = std::nullopt;
      PositionReopenedOnBar[I] = false;
      continue;
    }
    std::int64_t Running = PosBefore[I];
    bool TouchedZero = false;
    for (std::size_t FIdx = FillsBegin; FIdx < Fills.size(); ++FIdx) {
      const Fill &F = Fills[FIdx];
      if (F.Instrument != Instr)
        continue;
      Running += (F.OrderSide == Side::Buy) ? F.Lots : -F.Lots;
      if (PosBefore[I] > 0 && Running == 0)
        TouchedZero = true;
    }
    if (PosBefore[I] == 0 || TouchedZero) {
      BarsInPosition[I] = 0;
      PositionReopenedOnBar[I] = TouchedZero;
    } else {
      BarsInPosition[I] = BarsInPosition[I].value_or(0) + 1;
      PositionReopenedOnBar[I] = false;
    }
  }
}

PortfolioSnapshot SessionEngine::snapshot() const {
  PortfolioSnapshot S;
  S.Cash = PortfolioState.cash();
  S.Equity = Equity.back().Equity;
  const std::size_t N = Spec.Instruments.size();
  for (std::size_t I = 0; I < N; ++I) {
    S.Lots.push_back(PortfolioState.lots(static_cast<int>(I)));
    S.AvgPrice.push_back(PortfolioState.avgPrice(static_cast<int>(I)));
    S.CostBasis.push_back(PortfolioState.costBasis(static_cast<int>(I)));
  }
  S.MarketValue = PositionValues.back();
  S.BarsInPosition = BarsInPosition;
  S.PositionReopenedOnBar = PositionReopenedOnBar;
  return S;
}

StepEvents SessionEngine::lastStepEvents() const {
  StepEvents E;
  E.Step = Step;
  const auto Bars = TimelineRef->barsAt(Step);
  E.Bars.assign(Bars.begin(), Bars.end());
  E.Fills.assign(Fills.begin() + static_cast<std::ptrdiff_t>(FillsBegin),
                 Fills.end());
  std::vector<std::int64_t> Ids = Changed;
  std::sort(Ids.begin(), Ids.end());
  Ids.erase(std::unique(Ids.begin(), Ids.end()), Ids.end());
  for (const std::int64_t Id : Ids) {
    const auto It = std::lower_bound(
        Orders.begin(), Orders.end(), Id,
        [](const Order &O, std::int64_t Value) { return O.Id < Value; });
    if (It != Orders.end() && It->Id == Id)
      E.ChangedOrders.push_back(*It);
  }
  E.Portfolio = snapshot();
  return E;
}

ReplayResult replaySession(const SessionSpec &Spec,
                           std::shared_ptr<const Timeline> Timeline,
                           std::span<const JournalEntry> Journal,
                           std::int64_t UpToStep, std::int64_t EventsFromStep) {
  if (!Timeline)
    throw std::invalid_argument("replaySession: нет шкалы времени");
  if (UpToStep < 0 || UpToStep >= Timeline->totalSteps()) {
    throw std::invalid_argument("replaySession: upToStep вне шкалы времени");
  }
  // Журнал проверяется целиком до проигрывания: результат не зависит от того,
  // где он оборвётся.
  std::int64_t PrevStep = 0, PrevSeq = -1;
  for (const JournalEntry &E : Journal) {
    if (E.Step < PrevStep)
      throw std::invalid_argument("replaySession: шаг записи журнала убывает");
    if (E.Step > UpToStep)
      throw std::invalid_argument(
          "replaySession: шаг записи журнала больше upToStep");
    if (E.Seq < 0 || E.Seq <= PrevSeq)
      throw std::invalid_argument(
          "replaySession: seq записей журнала не возрастает");
    PrevStep = E.Step;
    PrevSeq = E.Seq;
  }

  SessionEngine Engine(Spec, std::move(Timeline));
  std::vector<StepEvents> Events;
  std::vector<CommandResult> Results;
  Results.reserve(Journal.size());
  std::size_t Next = 0;
  for (;;) {
    const std::int64_t K = Engine.currentStep();
    // Остановленная сессия принимает все оставшиеся записи (отказ
    // SessionNotActive).
    while (Next < Journal.size() && (Journal[Next].Step == K ||
                                     Engine.status() != SessionStatus::Running))
      Results.push_back(Engine.apply(Journal[Next++]));
    if (K >= EventsFromStep)
      Events.push_back(Engine.lastStepEvents());
    if (K == UpToStep || Engine.status() != SessionStatus::Running)
      break;
    Engine.advance();
  }

  const SessionStatus Status = Engine.status();
  const std::int64_t LastStep = Engine.currentStep();
  return ReplayResult{Status, LastStep, std::move(Events), std::move(Results),
                      std::move(Engine)};
}

} // namespace core
