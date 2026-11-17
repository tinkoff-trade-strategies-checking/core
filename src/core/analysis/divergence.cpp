#include "core/analysis/divergence.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

namespace core {

namespace {

// Окно «момента» W = 3 считается в шагах общей шкалы времени, а не в барах
// бумаги (при пропусках баров у бумаги окно короче). Намерения алгоритма,
// которые движок отклонил бы (нет денег или позиции), остаются его действиями:
// в копии они дают V = 0, но в сопоставлении участвуют («заявки, которые он
// создал бы»).
constexpr std::int64_t kWindow = 3;
constexpr std::size_t kMaxAlgorithms = 5; // ФТ-18
constexpr std::size_t kNone = static_cast<std::size_t>(-1);
const char *const kExcessReason = "алгоритм здесь не действовал бы";

bool isAction(const OrderSpec &Spec) {
  return Spec.Type == OrderType::Market || Spec.Type == OrderType::Limit;
}

std::int64_t signedLots(const OrderSpec &Spec) {
  return Spec.OrderSide == Side::Buy ? Spec.Lots : -Spec.Lots;
}

int signOf(std::int64_t Value) { return Value > 0 ? 1 : (Value < 0 ? -1 : 0); }

bool isActive(const Order &Order) {
  return Order.Status == OrderStatus::Active ||
         Order.Status == OrderStatus::PartiallyFilled;
}

// Действие на шаге по бумаге: заявки одной стороны суммируются, купленное и
// проданное - со знаком.
using ActionKey = std::pair<int, std::int64_t>; // (бумага, шаг)

struct Action {
  int Instrument = 0;
  std::int64_t Step = 0;
  std::int64_t Lots = 0; // купленное минус проданное; после finalize != 0
  std::vector<std::int64_t> OrderIds; // действие человека: его принятые заявки
  std::vector<OrderSpec>
      Specs; // действие алгоритма: заявки, которые он создал бы
  std::vector<std::pair<std::int64_t, std::string>>
      Parts; // (лоты со знаком, because) намерений алгоритма
  std::string ReasonCode;
};

using ActionMap = std::map<ActionKey, Action>;

Action &actionAt(ActionMap &Map, int Instrument, std::int64_t Step) {
  Action &Action = Map[ActionKey{Instrument, Step}];
  Action.Instrument = Instrument;
  Action.Step = Step;
  return Action;
}

// Убирает шаги, где купленное и проданное сошлись в ноль (действия нет);
// причина - у первого намерения в сторону итога.
void finalize(ActionMap &Map) {
  for (auto It = Map.begin(); It != Map.end();) {
    Action &Action = It->second;
    if (Action.Lots == 0) {
      It = Map.erase(It);
      continue;
    }
    for (const auto &[lots, because] : Action.Parts) {
      if (signOf(lots) == signOf(Action.Lots)) {
        Action.ReasonCode = because;
        break;
      }
    }
    ++It;
  }
}

bool sameSpec(const OrderSpec &A, const OrderSpec &B) {
  return A.Instrument == B.Instrument && A.OrderSide == B.OrderSide &&
         A.Type == B.Type && A.Lots == B.Lots && A.LimitPrice == B.LimitPrice &&
         A.StopPrice == B.StopPrice && A.TrailingPercent == B.TrailingPercent &&
         A.Tif == B.Tif;
}

// Размеры расходятся: |q_h - q_a| > 0,2 * max(|q_h|, |q_a|) (целочисленно, без
// double).
bool sizeDiffers(std::int64_t Human, std::int64_t Algo) {
  const std::int64_t Diff = Human > Algo ? Human - Algo : Algo - Human;
  const std::int64_t Larger =
      std::max(Human < 0 ? -Human : Human, Algo < 0 ? -Algo : Algo);
  return 5 * Diff > Larger;
}

// Состояние сессии человека на закрытии шага k и обход шагов. onStep(k, engine)
// - до записей журнала шага k, только пока сессия идёт; onEntry(entry, result)
// - после применения каждой записи журнала. stopAt - последний шаг. Записи
// остановленной сессии принимаются и отклоняются движком (как в replaySession).
template <typename OnStepT, typename OnEntryT>
SessionEngine walkHuman(const SessionSpec &Spec,
                        const std::shared_ptr<const Timeline> &Timeline,
                        std::span<const JournalEntry> Journal,
                        std::int64_t StopAt, OnStepT &&OnStep,
                        OnEntryT &&OnEntry) {
  SessionEngine Engine(Spec, Timeline);
  std::size_t Next = 0;
  for (;;) {
    const std::int64_t K = Engine.currentStep();
    if (Engine.status() == SessionStatus::Running)
      OnStep(K, static_cast<const SessionEngine &>(Engine));
    while (Next < Journal.size() &&
           (Journal[Next].Step == K ||
            Engine.status() != SessionStatus::Running)) {
      const CommandResult Result = Engine.apply(Journal[Next]);
      OnEntry(Journal[Next], Result);
      ++Next;
    }
    Engine.settleStep();
    if (K == StopAt || Engine.status() != SessionStatus::Running)
      break;
    Engine.advance();
  }
  return Engine;
}

// Ценность действия V: сумма по исполнениям заявок ids на шагах до horizon
// включительно: знак * лоты * размер лота * (close(horizon) - цена) - комиссия
// - сбор. Без исполнений - 0.
Decimal actionValue(const std::vector<Fill> &Fills,
                    const std::set<std::int64_t> &Ids, std::int64_t Horizon,
                    const Timeline &Timeline, const SessionSpec &Spec) {
  Decimal Value;
  for (const Fill &Fill : Fills) {
    if (Fill.Step > Horizon || Ids.count(Fill.OrderId) == 0)
      continue;
    const StepBar *Bar = Timeline.lastBarOf(Fill.Instrument, Horizon);
    if (Bar == nullptr)
      continue;
    const std::int64_t Units =
        Fill.Lots *
        Spec.Instruments[static_cast<std::size_t>(Fill.Instrument)].LotSize;
    Decimal Gain = (Bar->Close - Fill.Price).times(Units);
    if (Fill.OrderSide == Side::Sell)
      Gain = -Gain;
    Value += Gain - Fill.Commission - Fill.Fee;
  }
  return Value;
}

// Расхождение до подсчёта цены: V человека готова, V алгоритма считается копией
// движка на шаге snapStep.
struct Pending {
  Divergence Result;
  std::size_t Algorithm = 0;
  const Action *Human = nullptr; // действие человека; nullptr - «упущенное»
  std::int64_t SnapStep =
      -1; // шаг копии движка; -1 - у алгоритма нет действия, V алгоритма = 0
  std::int64_t PercentStep =
      0; // шаг, по стоимости портфеля человека на котором считается процент
  std::vector<OrderSpec> AlgoSpecs; // заявки алгоритма
  Decimal HumanValue, AlgoValue;
};

// Что стало с действием человека у одного алгоритма.
struct HumanOutcome {
  bool Matched =
      false; // сопоставлено (точное совпадение, «размер» или «момент»)
  std::size_t PendingIndex = kNone; // расхождение; kNone - точное совпадение
};

Decimal medianOf(std::vector<Decimal> Costs) {
  if (Costs.empty())
    return Decimal();
  std::sort(Costs.begin(), Costs.end());
  const std::size_t N = Costs.size();
  if (N % 2 == 1)
    return Costs[N / 2];
  return (Costs[N / 2 - 1] + Costs[N / 2])
      .div(Decimal::fromUnits(2), 2, RoundingMode::HalfUp);
}

class Analyzer {
public:
  Analyzer(const SessionSpec &SessionConfig, std::shared_ptr<const Timeline> Tl,
           std::span<const JournalEntry> Entries, std::int64_t UpToStep,
           const std::vector<AlgorithmInput> &Inputs)
      : Spec(SessionConfig), TimelineRef(std::move(Tl)), Journal(Entries),
        LastStep(UpToStep), Algorithms(Inputs) {}

  DivergenceResult run() {
    validate();
    collectActions();
    for (std::size_t A = 0; A < Algorithms.size(); ++A)
      match(A);
    priceHumanSide();
    priceAlgorithmSide();
    DivergenceResult Result;
    assembleDivergences(Result);
    assembleMissedTrades(Result);
    assembleConsensus(Result);
    for (const AlgorithmInput &Input : Algorithms) {
      // На том же отрезке, что и человек: по шагам 0…lastStep (сессия могла
      // закончиться раньше конца шкалы).
      Result.Independent.push_back(detail::runBacktestToStep(
          Spec, TimelineRef, Input.Program, Input.Params, Input.Values,
          dsl::Level::Ok, LastStep));
    }
    return Result;
  }

private:
  void validate() const {
    if (!TimelineRef)
      throw std::invalid_argument("analyzeDivergences: нет шкалы времени");
    if (Algorithms.size() > kMaxAlgorithms) {
      throw std::invalid_argument("analyzeDivergences: алгоритмов больше " +
                                  std::to_string(kMaxAlgorithms));
    }
    std::set<std::string> Ids;
    for (const AlgorithmInput &Input : Algorithms) {
      if (!Ids.insert(Input.Id).second)
        throw std::invalid_argument(
            "analyzeDivergences: повтор id алгоритма: " + Input.Id);
    }
    if (Spec.Naive)
      throw std::invalid_argument("analyzeDivergences: разбор только по "
                                  "честному исполнению (не naive)");
    if (LastStep < 0 || LastStep >= TimelineRef->totalSteps()) {
      throw std::invalid_argument(
          "analyzeDivergences: lastStep вне шкалы времени");
    }
    std::int64_t PrevStep = 0, PrevSeq = -1;
    for (const JournalEntry &E : Journal) {
      if (E.Step < PrevStep)
        throw std::invalid_argument(
            "analyzeDivergences: шаг записи журнала убывает");
      if (E.Step > LastStep)
        throw std::invalid_argument(
            "analyzeDivergences: шаг записи журнала больше lastStep");
      if (E.Seq < 0 || E.Seq <= PrevSeq)
        throw std::invalid_argument(
            "analyzeDivergences: seq записей журнала не возрастает");
      PrevStep = E.Step;
      PrevSeq = E.Seq;
    }
  }

  // Проход 1: сессия человека по шагам; на каждом шаге каждый алгоритм видит
  // бар шага и портфель человека на закрытии.
  void collectActions() {
    std::vector<dsl::StrategyInstance> Strategies;
    Strategies.reserve(Algorithms.size());
    for (const AlgorithmInput &Input : Algorithms) {
      Strategies.emplace_back(Input.Program, Input.Params, Input.Values,
                              Spec.Instruments.size());
      detail::warmUp(Strategies.back(), *TimelineRef,
                     Spec); // история до шага 0 - как в независимом прогоне
    }
    AlgoActions.assign(Algorithms.size(), {});
    std::vector<std::optional<std::int64_t>> BarsInPosition(
        Spec.Instruments.size());
    // Прошлый бар каждой бумаги и последнее намерение правила (по алгоритму):
    // повтор того же намерения на соседнем баре - та же заявка, а не новое
    // действие.
    std::vector<std::optional<std::int64_t>> PreviousBarStep(
        Spec.Instruments.size());
    std::vector<std::int32_t> PreviousBarDay(Spec.Instruments.size(), 0);
    struct Previous {
      OrderSpec Spec;
      std::int64_t Step;
    };
    std::vector<std::map<std::pair<std::string, int>, Previous>> PreviousByKey(
        Algorithms.size());

    const auto OnStep = [&](std::int64_t K, const SessionEngine &Engine) {
      for (std::size_t I = 0; I < Spec.Instruments.size(); ++I) {
        const int Instrument = static_cast<int>(I);
        const StepBar *Bar = TimelineRef->barOf(
            Instrument, K); // только бар текущего шага (INV-1)
        if (Bar == nullptr)
          continue;
        auto &Bars = BarsInPosition[I];
        if (Engine.portfolio().lots(Instrument) > 0) {
          Bars = Bars ? *Bars + 1 : 0;
        } else {
          Bars.reset();
        }
        dsl::StateView State;
        State.PositionLots = Engine.portfolio().lots(Instrument);
        State.Cash = Engine.portfolio().cash().toDouble();
        State.Equity = Engine.equity().back().Equity.toDouble();
        if (State.PositionLots > 0) {
          State.EntryPrice = Engine.portfolio().avgPrice(Instrument).toDouble();
          State.BarsInPosition = Bars;
        }
        for (std::size_t A = 0; A < Strategies.size(); ++A) {
          Strategies[A].onBar(Instrument, *Bar);
          const std::vector<dsl::Intent> Intents = Strategies[A].evaluate(
              Instrument, *Bar, Spec.Instruments[I].LotSize,
              Spec.Instruments[I].PriceStep, State);
          for (const dsl::Intent &Intent : Intents) {
            if (Intent.CancelAll || !isAction(Intent.Order))
              continue;
            // Лимитная заявка, пока не исполнилась, стоит: то же намерение того
            // же правила на следующем баре (для заявки «день» - в тот же
            // торговый день) - не новое действие. Исполнилась бы она или нет в
            // мире «как алгоритм», здесь неизвестно, поэтому повтор считается
            // продолжением. Рыночные заявки исполняются на следующем баре и
            // повторяются как новые действия.
            const auto Key = std::make_pair(Intent.RuleName, Instrument);
            const auto Found = PreviousByKey[A].find(Key);
            const bool Repeat = Found != PreviousByKey[A].end() &&
                                PreviousBarStep[I] &&
                                Found->second.Step == *PreviousBarStep[I] &&
                                Intent.Order.Type == OrderType::Limit &&
                                sameSpec(Found->second.Spec, Intent.Order) &&
                                (Intent.Order.Tif == TimeInForce::Gtc ||
                                 PreviousBarDay[I] == Bar->TradingDay);
            PreviousByKey[A][Key] = Previous{Intent.Order, K};
            if (Repeat)
              continue;
            Action &Action = actionAt(AlgoActions[A], Instrument, K);
            Action.Lots += signedLots(Intent.Order);
            Action.Specs.push_back(Intent.Order);
            Action.Parts.emplace_back(signedLots(Intent.Order), Intent.Because);
          }
        }
        PreviousBarStep[I] = K;
        PreviousBarDay[I] = Bar->TradingDay;
      }
    };
    // Действие человека - принятая движком подача или замена рыночной либо
    // лимитной заявки. Заявка, поданная на этом же шаге и сразу заменённая или
    // снятая, действием не остаётся: считается только то, что осталось. Заявка
    // с OCO (защитная связка) в сравнении не участвует. Отклонённые движком
    // подачи действиями не считаются.
    const auto OnEntry = [&](const JournalEntry &Entry,
                             const CommandResult &Result) {
      if (!Result.Accepted)
        return;
      const PlaceCmd *Place = std::get_if<PlaceCmd>(&Entry.Command);
      std::int64_t Gone = -1;
      if (const ReplaceCmd *Replace = std::get_if<ReplaceCmd>(&Entry.Command)) {
        Place = &Replace->Replacement;
        Gone = Replace->OrderId;
      } else if (const CancelCmd *Cancel =
                     std::get_if<CancelCmd>(&Entry.Command)) {
        Gone = Cancel->OrderId;
      }
      if (Gone >= 0) {
        const auto Old = HumanOrderToAction.find(Gone);
        if (Old != HumanOrderToAction.end() &&
            Old->second.second == Entry.Step) {
          Action &Action = HumanActions.at(Old->second);
          Action.Lots -= HumanOrderLots.at(Gone);
          Action.OrderIds.erase(
              std::find(Action.OrderIds.begin(), Action.OrderIds.end(), Gone));
          HumanOrderLots.erase(Gone);
          HumanOrderToAction.erase(Old);
        }
      }
      if (Place == nullptr || Result.OrderIds.empty() || Place->OcoWith ||
          !isAction(Place->Order))
        return;
      Action &Action =
          actionAt(HumanActions, Place->Order.Instrument, Entry.Step);
      Action.Lots += signedLots(Place->Order);
      Action.OrderIds.push_back(Result.OrderIds.front());
      HumanOrderToAction[Result.OrderIds.front()] =
          ActionKey{Place->Order.Instrument, Entry.Step};
      HumanOrderLots[Result.OrderIds.front()] = signedLots(Place->Order);
    };
    Human.emplace(
        walkHuman(Spec, TimelineRef, Journal, LastStep, OnStep, OnEntry));
    LastSession = Human->currentStep();
    finalize(HumanActions);
    for (ActionMap &Map : AlgoActions)
      finalize(Map);
    // Заявки, вошедшие в действие с нулевым итогом, действием не считаются.
    for (auto It = HumanOrderToAction.begin();
         It != HumanOrderToAction.end();) {
      It = HumanActions.count(It->second) == 0 ? HumanOrderToAction.erase(It)
                                               : std::next(It);
    }
  }

  // Горизонт e: следующий шаг после t, на котором человек или этот алгоритм
  // действует по бумаге; иначе последний шаг сессии.
  std::int64_t horizonAfter(std::size_t Algorithm, int Instrument,
                            std::int64_t T) const {
    std::optional<std::int64_t> Best;
    const auto Consider = [&](const ActionMap &Map) {
      const auto It = Map.upper_bound(ActionKey{Instrument, T});
      if (It != Map.end() && It->first.first == Instrument &&
          (!Best || It->first.second < *Best)) {
        Best = It->first.second;
      }
    };
    Consider(HumanActions);
    Consider(AlgoActions[Algorithm]);
    return Best ? *Best : LastSession;
  }

  // Сопоставление действий человека и алгоритма (`decisions.md`,
  // «Сопоставление»), бумага за бумагой.
  void match(std::size_t A) {
    for (std::size_t I = 0; I < Spec.Instruments.size(); ++I) {
      const int Instrument = static_cast<int>(I);
      std::vector<const Action *> Hs, As;
      for (const auto &[key, action] : HumanActions) {
        if (key.first == Instrument)
          Hs.push_back(&action);
      }
      for (const auto &[key, action] : AlgoActions[A]) {
        if (key.first == Instrument)
          As.push_back(&action);
      }
      std::vector<char> HMatched(Hs.size(), 0), AMatched(As.size(), 0);
      std::vector<std::size_t> HPending(Hs.size(), kNone);

      // 1. Один шаг и один знак - совпадение; «размер», если количества
      // расходятся больше чем на 20 %.
      for (std::size_t H = 0; H < Hs.size(); ++H) {
        for (std::size_t K = 0; K < As.size(); ++K) {
          if (AMatched[K] || As[K]->Step != Hs[H]->Step ||
              signOf(As[K]->Lots) != signOf(Hs[H]->Lots))
            continue;
          HMatched[H] = AMatched[K] = 1;
          Outcome[{A, Hs[H]->Instrument, Hs[H]->Step}].Matched = true;
          if (sizeDiffers(Hs[H]->Lots, As[K]->Lots)) {
            HPending[H] =
                makePending(A, DivergenceType::Size, Hs[H], As[K], Hs[H]->Step);
          }
          break;
        }
      }
      // 2. «Момент»: ближайшее действие алгоритма того же знака, 1 <= |Δt| <=
      // W; при равенстве - более раннее.
      for (std::size_t H = 0; H < Hs.size(); ++H) {
        if (HMatched[H])
          continue;
        std::size_t Best = kNone;
        std::int64_t BestDistance = 0;
        for (std::size_t K = 0; K < As.size(); ++K) {
          if (AMatched[K] || signOf(As[K]->Lots) != signOf(Hs[H]->Lots))
            continue;
          const std::int64_t Distance = As[K]->Step > Hs[H]->Step
                                            ? As[K]->Step - Hs[H]->Step
                                            : Hs[H]->Step - As[K]->Step;
          if (Distance < 1 || Distance > kWindow)
            continue;
          if (Best == kNone || Distance < BestDistance) {
            Best = K;
            BestDistance = Distance;
          }
        }
        if (Best == kNone)
          continue;
        HMatched[H] = AMatched[Best] = 1;
        Outcome[{A, Hs[H]->Instrument, Hs[H]->Step}].Matched = true;
        HPending[H] = makePending(A, DivergenceType::Timing, Hs[H], As[Best],
                                  std::max(Hs[H]->Step, As[Best]->Step));
      }
      // 3. Остальное: противоположные действия одного шага - одно «лишнее»;
      // прочие - «лишнее» и «упущенное».
      for (std::size_t H = 0; H < Hs.size(); ++H) {
        if (HMatched[H])
          continue;
        const Action *Opposite = nullptr;
        for (std::size_t K = 0; K < As.size(); ++K) {
          if (!AMatched[K] && As[K]->Step == Hs[H]->Step) {
            AMatched[K] = 1;
            Opposite = As[K];
            break;
          }
        }
        HPending[H] = makePending(A, DivergenceType::Excess, Hs[H], Opposite,
                                  Hs[H]->Step);
      }
      for (std::size_t K = 0; K < As.size(); ++K) {
        if (!AMatched[K])
          makePending(A, DivergenceType::Missed, nullptr, As[K], As[K]->Step);
      }
      for (std::size_t H = 0; H < Hs.size(); ++H)
        Outcome[{A, Hs[H]->Instrument, Hs[H]->Step}].PendingIndex = HPending[H];
    }
  }

  std::size_t makePending(std::size_t A, DivergenceType Type,
                          const Action *HumanAct, const Action *Algo,
                          std::int64_t At) {
    Pending P;
    P.Algorithm = A;
    P.Human = HumanAct;
    Divergence &D = P.Result;
    D.AlgorithmId = Algorithms[A].Id;
    D.Instrument =
        HumanAct != nullptr ? HumanAct->Instrument : Algo->Instrument;
    D.Type = Type;
    if (HumanAct != nullptr) {
      D.HumanStep = HumanAct->Step;
      D.HumanLots = HumanAct->Lots;
    }
    if (Algo != nullptr) {
      D.AlgoStep = Algo->Step;
      D.AlgoLots = Algo->Lots;
      P.AlgoSpecs = Algo->Specs;
      P.SnapStep =
          Algo->Step; // копия движка человека на шаге действия алгоритма
    }
    // Горизонт и процент - от шага расхождения: у «момента» это более поздний
    // из двух шагов.
    D.HorizonStep = horizonAfter(A, D.Instrument, At);
    P.PercentStep = At;
    D.ReasonCode = Type == DivergenceType::Excess
                       ? kExcessReason
                       : (Algo != nullptr ? Algo->ReasonCode : "");
    PendingList.push_back(std::move(P));
    return PendingList.size() - 1;
  }

  // V человека: фактические исполнения его заявок действия на шагах до
  // горизонта.
  void priceHumanSide() {
    for (Pending &P : PendingList) {
      if (P.Human == nullptr)
        continue;
      const std::set<std::int64_t> Ids(P.Human->OrderIds.begin(),
                                       P.Human->OrderIds.end());
      P.HumanValue = actionValue(Human->fills(), Ids, P.Result.HorizonStep,
                                 *TimelineRef, Spec);
    }
  }

  // Проход 2: V алгоритма - копия сессии человека на шаге действия алгоритма,
  // активные заявки человека сняты, заявки алгоритма применены, затем advance()
  // до горизонта (INV-3: тот же движок).
  void priceAlgorithmSide() {
    std::map<std::int64_t, std::vector<std::size_t>> Jobs;
    for (std::size_t Index = 0; Index < PendingList.size(); ++Index) {
      if (PendingList[Index].SnapStep >= 0)
        Jobs[PendingList[Index].SnapStep].push_back(Index);
    }
    if (!Jobs.empty()) {
      const std::int64_t SeqBase =
          (Journal.empty() ? 0 : Journal.back().Seq) + 1;
      const auto OnStep = [&](std::int64_t K, const SessionEngine &Engine) {
        const auto Found = Jobs.find(K);
        if (Found == Jobs.end())
          return;
        for (const std::size_t Index : Found->second)
          PendingList[Index].AlgoValue =
              algorithmValue(Engine, PendingList[Index], SeqBase);
      };
      walkHuman(Spec, TimelineRef, Journal, Jobs.rbegin()->first, OnStep,
                [](const JournalEntry &, const CommandResult &) {});
    }
    for (Pending &P : PendingList) {
      Divergence &D = P.Result;
      D.Cost = (P.AlgoValue - P.HumanValue).round(2, RoundingMode::HalfUp);
      const auto &Equity = Human->equity();
      const std::size_t At = static_cast<std::size_t>(P.PercentStep);
      const double Total =
          At < Equity.size() ? Equity[At].Equity.toDouble() : 0;
      D.CostPercent = Total > 0 ? D.Cost.toDouble() / Total * 100.0 : 0.0;
    }
  }

  Decimal algorithmValue(const SessionEngine &At, const Pending &P,
                         std::int64_t SeqBase) const {
    SessionEngine Copy = At;
    std::int64_t Seq = SeqBase;
    const std::int64_t Step = Copy.currentStep();
    for (const Order &Order : At.orders()) {
      if (!isActive(Order))
        continue;
      JournalEntry Cancel;
      Cancel.Seq = Seq++;
      Cancel.Step = Step;
      Cancel.Command = CancelCmd{Order.Id, Reason::CancelledByUser};
      Copy.apply(Cancel);
    }
    std::set<std::int64_t> Ids;
    for (const OrderSpec &Leg : P.AlgoSpecs) {
      JournalEntry Entry;
      Entry.Seq = Seq++;
      Entry.Step = Step;
      Entry.Command = PlaceCmd{Leg, std::nullopt};
      const CommandResult Result = Copy.apply(Entry);
      Ids.insert(Result.OrderIds.begin(), Result.OrderIds.end());
    }
    while (Copy.currentStep() < P.Result.HorizonStep &&
           Copy.status() == SessionStatus::Running)
      Copy.advance();
    return actionValue(Copy.fills(), Ids, P.Result.HorizonStep, *TimelineRef,
                       Spec);
  }

  void assembleDivergences(DivergenceResult &Result) const {
    for (std::size_t A = 0; A < Algorithms.size(); ++A) {
      std::vector<const Divergence *> Mine;
      for (const Pending &P : PendingList) {
        if (P.Algorithm == A)
          Mine.push_back(&P.Result);
      }
      const auto Primary = [](const Divergence &D) {
        return D.HumanStep >= 0 ? D.HumanStep : D.AlgoStep;
      };
      std::stable_sort(Mine.begin(), Mine.end(),
                       [&](const Divergence *X, const Divergence *Y) {
                         return std::make_tuple(Primary(*X), X->Instrument,
                                                static_cast<int>(X->Type)) <
                                std::make_tuple(Primary(*Y), Y->Instrument,
                                                static_cast<int>(Y->Type));
                       });
      for (const Divergence *D : Mine)
        Result.Divergences.push_back(*D);
    }
  }

  // ФТ-20: закрытая сделка человека с прибылью после издержек, вход которой -
  // «лишнее действие» алгоритма.
  void assembleMissedTrades(DivergenceResult &Result) const {
    for (const RoundTrip &Trip : Human->portfolio().roundTrips()) {
      if (Trip.NetPnl.sign() <= 0)
        continue;
      const auto Entry = HumanOrderToAction.find(Trip.EntryOrderId);
      if (Entry == HumanOrderToAction.end())
        continue;
      MissedProfitableTrade Missed;
      Missed.Instrument = Trip.Instrument;
      Missed.EntryStep = Trip.EntryStep;
      Missed.ExitStep = Trip.ExitStep;
      Missed.Lots = Trip.Lots;
      Missed.NetPnl = Trip.NetPnl;
      const std::int64_t LotSize =
          Spec.Instruments.at(static_cast<std::size_t>(Trip.Instrument))
              .LotSize;
      const Decimal Units = Decimal::fromUnits(Trip.Lots * LotSize);
      Missed.EntryPrice = Trip.EntryValue.div(Units, RoundingMode::HalfUp);
      Missed.ExitPrice = Trip.ExitValue.div(Units, RoundingMode::HalfUp);
      for (std::size_t A = 0; A < Algorithms.size(); ++A) {
        const auto Found =
            Outcome.find({A, Entry->second.first, Entry->second.second});
        if (Found != Outcome.end() && !Found->second.Matched)
          Missed.AlgorithmIds.push_back(Algorithms[A].Id);
      }
      if (!Missed.AlgorithmIds.empty())
        Result.Missed.push_back(std::move(Missed));
    }
  }

  void assembleConsensus(DivergenceResult &Result) const {
    const std::int32_t Total = static_cast<std::int32_t>(Algorithms.size());
    if (Total == 0)
      return;
    std::vector<ConsensusItem> HumanItems;
    for (const auto &[key, action] : HumanActions) {
      std::int32_t Agreeing = 0;
      std::vector<Decimal> Costs;
      for (std::size_t A = 0; A < Algorithms.size(); ++A) {
        const auto Found = Outcome.find({A, key.first, key.second});
        if (Found == Outcome.end())
          continue;
        if (Found->second.Matched) {
          ++Agreeing;
        } else if (Found->second.PendingIndex != kNone) {
          // Медиана - по тем, кто «здесь не действовал бы» (смысл сообщения):
          // «размер» и «момент» в неё не входят.
          Costs.push_back(PendingList[Found->second.PendingIndex].Result.Cost);
        }
      }
      if (Agreeing == Total)
        continue;
      HumanItems.push_back(ConsensusItem{
          key.first, key.second, action.Lots > 0 ? Side::Buy : Side::Sell, true,
          Agreeing, Total, medianOf(Costs)});
    }
    std::sort(HumanItems.begin(), HumanItems.end(),
              [](const ConsensusItem &X, const ConsensusItem &Y) {
                return std::make_tuple(X.Step, X.Instrument) <
                       std::make_tuple(Y.Step, Y.Instrument);
              });

    // Упущенные действия: (бумага, шаг, сторона) -> цены ошибки алгоритмов,
    // которые действовали бы здесь.
    std::map<std::tuple<std::int64_t, int, int>, std::vector<Decimal>> Missed;
    for (const Pending &P : PendingList) {
      const Divergence &D = P.Result;
      if (D.Type != DivergenceType::Missed)
        continue;
      Missed[{D.AlgoStep, D.Instrument, D.AlgoLots > 0 ? 0 : 1}].push_back(
          D.Cost);
    }
    for (auto &Item : HumanItems)
      Result.Consensus.push_back(std::move(Item));
    for (const auto &[key, costs] : Missed) {
      const auto &[step, instrument, sideIndex] = key;
      Result.Consensus.push_back(ConsensusItem{
          instrument, step, sideIndex == 0 ? Side::Buy : Side::Sell, false,
          static_cast<std::int32_t>(costs.size()), Total, medianOf(costs)});
    }
  }

  const SessionSpec &Spec;
  std::shared_ptr<const Timeline> TimelineRef;
  std::span<const JournalEntry> Journal;
  std::int64_t LastStep;
  const std::vector<AlgorithmInput> &Algorithms;

  ActionMap HumanActions;
  std::vector<ActionMap> AlgoActions;
  std::map<std::int64_t, ActionKey>
      HumanOrderToAction; // id заявки человека -> его действие
  std::map<std::int64_t, std::int64_t>
      HumanOrderLots; // id заявки человека -> лоты со знаком
  std::optional<SessionEngine> Human; // сессия человека на последнем шаге
  std::int64_t LastSession = 0;
  std::vector<Pending> PendingList;
  std::map<std::tuple<std::size_t, int, std::int64_t>, HumanOutcome>
      Outcome; // (алгоритм, бумага, шаг человека)
};

} // namespace

DivergenceResult
analyzeDivergences(const SessionSpec &Spec,
                   std::shared_ptr<const Timeline> Timeline,
                   std::span<const JournalEntry> Human, std::int64_t LastStep,
                   const std::vector<AlgorithmInput> &Algorithms) {
  return Analyzer(Spec, std::move(Timeline), Human, LastStep, Algorithms).run();
}

} // namespace core
