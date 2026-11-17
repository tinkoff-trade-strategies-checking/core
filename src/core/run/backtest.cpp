#include "core/run/backtest.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace core {

namespace {

bool isActive(const Order &O) {
  return O.Status == OrderStatus::Active ||
         O.Status == OrderStatus::PartiallyFilled;
}

bool sameSpec(const OrderSpec &A, const OrderSpec &B) {
  return A.Instrument == B.Instrument && A.OrderSide == B.OrderSide &&
         A.Type == B.Type && A.Lots == B.Lots && A.LimitPrice == B.LimitPrice &&
         A.StopPrice == B.StopPrice && A.TrailingPercent == B.TrailingPercent &&
         A.Tif == B.Tif;
}

// Прогон одной стратегии: состояние цикла собрано в класс, чтобы цикл читался
// сверху вниз.
class Runner {
public:
  Runner(const SessionSpec &SessionConfig,
         std::shared_ptr<const Timeline> Timeline, const dsl::Program &Program,
         const std::vector<dsl::ResolvedParam> &Params,
         const dsl::ParamValues &Overrides, const Progress &Progress,
         const detail::StepHook &StepHookRef, std::int64_t LastStepIndex)
      : Spec(SessionConfig), TimelineRef(Timeline),
        Engine(SessionConfig, std::move(Timeline)),
        Strategy(Program, Params, Overrides, SessionConfig.Instruments.size()),
        ProgressFn(Progress), Hook(StepHookRef), LastStep(LastStepIndex),
        BarsInPosition(SessionConfig.Instruments.size()) {}

  RunOutput run(dsl::Level StaticLevel) {
    RunOutput Out;
    std::optional<Finding> Violation;
    detail::warmUp(Strategy, *TimelineRef, Spec);
    try {
      loop();
    } catch (const IntegrityViolation &E) {
      Violation = detail::integrityFinding(E, Engine.currentStep());
      // Последний отсчёт прогресса уходит и при остановке: done - число шагов с
      // точкой кривой стоимости.
      reportProgress(Engine.equity().back().Step + 1);
    }
    collect(Out, Violation.has_value());
    if (Violation) {
      Out.Findings.push_back(*Violation);
    } else {
      Out.Findings = detail::runtimeFindings(*TimelineRef, Out.Orders,
                                             Out.Fills, Out.RoundTrips);
    }
    Out.Severity = detail::worstLevel(StaticLevel, Out.Findings);
    Out.Digest = digestOf(Out);
    return Out;
  }

private:
  static constexpr std::int64_t kProgressEvery = 1000;

  void loop() {
    for (;;) {
      const std::int64_t K = Engine.currentStep();
      if (Hook)
        Hook(Engine);
      if (Engine.status() == SessionStatus::Running) {
        cancelFlatSells(K);
        decide(K);
      }
      // Прогон до lastStep: на нём стратегия ещё решает (заявка не исполнится),
      // дальше шаги не обрабатываются.
      const bool Stop =
          Engine.status() != SessionStatus::Running || K >= LastStep;
      if (!Stop && (K + 1) % kProgressEvery == 0)
        reportProgress(K + 1);
      if (Stop)
        break;
      Engine.advance();
    }
    reportProgress(Engine.currentStep() + 1);
  }

  // Отсчёт прогресса без повтора одного и того же значения.
  void reportProgress(std::int64_t Done) {
    if (!ProgressFn || Done == ReportedDone)
      return;
    ReportedDone = Done;
    ProgressFn(Done, TimelineRef->totalSteps());
  }

  const Order *order(std::int64_t Id) const {
    const auto &Orders = Engine.orders();
    const auto It = std::lower_bound(
        Orders.begin(), Orders.end(), Id,
        [](const Order &O, std::int64_t Value) { return O.Id < Value; });
    return It != Orders.end() && It->Id == Id ? &*It : nullptr;
  }

  // Запись журнала стратегии и её применение тем же путём, что у журнала
  // человека (INV-3).
  template <typename CommandT>
  CommandResult apply(std::int64_t Step, CommandT Command,
                      const std::string &Rule, const std::string &Because) {
    JournalEntry Entry;
    Entry.Seq = Seq++;
    Entry.Step = Step;
    Entry.Command = std::move(Command);
    Entry.RuleName = Rule;
    Entry.Because = Because;
    Journal.push_back(Entry);
    return Engine.apply(Entry);
  }

  // После шага: по бумагам с нулевой позицией активные продажи стратегии
  // снимаются.
  void cancelFlatSells(std::int64_t K) {
    for (auto It = Book.begin(); It != Book.end();) {
      const Order *O = order(It->second);
      if (O == nullptr || !isActive(*O)) {
        It = Book.erase(It);
      } else if (O->Spec.OrderSide == Side::Sell &&
                 Engine.portfolio().lots(O->Spec.Instrument) == 0) {
        const std::string Rule = O->RuleName;
        apply(K, CancelCmd{O->Id, Reason::PositionClosed}, Rule,
              "позиция закрыта");
        It = Book.erase(It);
      } else {
        ++It;
      }
    }
  }

  void decide(std::int64_t K) {
    for (std::size_t I = 0; I < Spec.Instruments.size(); ++I) {
      const int Instrument = static_cast<int>(I);
      const StepBar *Bar =
          TimelineRef->barOf(Instrument, K); // только бар текущего шага (INV-1)
      if (Bar == nullptr)
        continue;
      Strategy.onBar(Instrument, *Bar);
      updateBarsInPosition(Instrument);
      const std::vector<dsl::Intent> Intents =
          Strategy.evaluate(Instrument, *Bar, Spec.Instruments[I].LotSize,
                            Spec.Instruments[I].PriceStep, state(Instrument));
      for (const dsl::Intent &Intent : Intents)
        handle(K, Intent);
    }
  }

  // Раз на каждый бар бумаги, до evaluate: баров с открытия позиции (0 -
  // открыта на этом баре); позиции нет - сброс.
  void updateBarsInPosition(int Instrument) {
    auto &Bars = BarsInPosition[static_cast<std::size_t>(Instrument)];
    if (Engine.portfolio().lots(Instrument) > 0) {
      Bars = Bars ? *Bars + 1 : 0;
    } else {
      Bars.reset();
    }
  }

  // Чтение состояния на закрытии бара, без побочных эффектов.
  dsl::StateView state(int Instrument) const {
    dsl::StateView S;
    const Portfolio &P = Engine.portfolio();
    S.PositionLots = P.lots(Instrument);
    S.Cash = P.cash().toDouble();
    S.Equity = Engine.equity().back().Equity.toDouble();
    if (S.PositionLots > 0) {
      S.EntryPrice = P.avgPrice(Instrument).toDouble();
      S.BarsInPosition = BarsInPosition[static_cast<std::size_t>(Instrument)];
    }
    return S;
  }

  void handle(std::int64_t K, const dsl::Intent &Intent) {
    if (Intent.CancelAll) {
      for (auto It = Book.begin(); It != Book.end();) {
        const Order *O = order(It->second);
        if (O == nullptr || !isActive(*O)) {
          It = Book.erase(It);
        } else if (O->Spec.Instrument == Intent.Instrument) {
          apply(K, CancelCmd{O->Id, Reason::CancelledByUser}, Intent.RuleName,
                Intent.Because);
          It = Book.erase(It);
        } else {
          ++It;
        }
      }
      return;
    }
    const auto Key = std::make_pair(Intent.RuleName, Intent.Instrument);
    const auto Found = Book.find(Key);
    if (Found != Book.end()) {
      const Order *Old = order(Found->second);
      if (Old != nullptr && isActive(*Old)) {
        if (sameSpec(Old->Spec, Intent.Order))
          return; // та же заявка уже стоит
        // Замена атомарна (ReplaceCmd, причина RuleReplaced): отклонённая новая
        // заявка не отнимает у правила прежнюю.
        const CommandResult Replaced =
            apply(K, ReplaceCmd{Old->Id, PlaceCmd{Intent.Order, std::nullopt}},
                  Intent.RuleName, Intent.Because);
        if (Replaced.Accepted)
          Found->second = Replaced.OrderIds.front();
        return;
      }
      Book.erase(Found);
    }
    const CommandResult Result = apply(K, PlaceCmd{Intent.Order, std::nullopt},
                                       Intent.RuleName, Intent.Because);
    if (Result.Accepted)
      Book[Key] = Result.OrderIds.front();
  }

  void collect(RunOutput &Out, bool Invalid) {
    Out.Orders = Engine.orders();
    Out.Fills = Engine.fills();
    Out.Equity = Engine.equity();
    Out.RoundTrips = Engine.portfolio().roundTrips();
    Out.PositionValuesByStep = Engine.positionValues();
    Out.Journal = Journal;
    Out.FinalEquity = Out.Equity.back().Equity;
    Out.LastStep = Out.Equity.back().Step;
    Out.Status = Invalid ? RunStatus::Invalid
                         : (Engine.status() == SessionStatus::Bankrupt
                                ? RunStatus::BankruptStop
                                : RunStatus::Completed);
    for (const Order &O : Out.Orders) {
      if (O.Status == OrderStatus::Rejected)
        ++Out.Rejections;
      if (O.FilledLots > 0 && O.FilledLots < O.Spec.Lots)
        ++Out.PartialFills;
    }
  }

  const SessionSpec &Spec;
  std::shared_ptr<const Timeline> TimelineRef;
  SessionEngine Engine;
  dsl::StrategyInstance Strategy;
  const Progress &ProgressFn;
  const detail::StepHook &Hook;
  std::int64_t LastStep;
  std::map<std::pair<std::string, int>, std::int64_t>
      Book; // (правило, бумага) -> id активной заявки
  std::vector<std::optional<std::int64_t>> BarsInPosition;
  std::vector<JournalEntry> Journal;
  std::int64_t Seq = 0;
  std::int64_t ReportedDone = -1;
};

} // namespace

namespace detail {

RunOutput runBacktestWithHook(const SessionSpec &Spec,
                              std::shared_ptr<const Timeline> Timeline,
                              const dsl::Program &Program,
                              const std::vector<dsl::ResolvedParam> &Params,
                              const dsl::ParamValues &Overrides,
                              dsl::Level StaticLevel, const Progress &Progress,
                              const StepHook &Hook) {
  if (!Timeline)
    throw std::invalid_argument("runBacktest: нет шкалы времени");
  const std::int64_t LastStep = Timeline->totalSteps() - 1;
  Runner Runner(Spec, std::move(Timeline), Program, Params, Overrides, Progress,
                Hook, LastStep);
  return Runner.run(StaticLevel);
}

RunOutput runBacktestToStep(const SessionSpec &Spec,
                            std::shared_ptr<const Timeline> Timeline,
                            const dsl::Program &Program,
                            const std::vector<dsl::ResolvedParam> &Params,
                            const dsl::ParamValues &Overrides,
                            dsl::Level StaticLevel, std::int64_t LastStep) {
  if (!Timeline)
    throw std::invalid_argument("runBacktestToStep: нет шкалы времени");
  if (LastStep < 0 || LastStep >= Timeline->totalSteps()) {
    throw std::invalid_argument(
        "runBacktestToStep: lastStep вне шкалы времени");
  }
  const Progress NoProgress;
  const StepHook NoHook;
  Runner Runner(Spec, std::move(Timeline), Program, Params, Overrides,
                NoProgress, NoHook, LastStep);
  return Runner.run(StaticLevel);
}

void warmUp(dsl::StrategyInstance &Strategy, const Timeline &Timeline,
            const SessionSpec &Spec) {
  dsl::StateView Flat;
  Flat.Cash = Spec.InitialCash.toDouble();
  Flat.Equity = Flat.Cash;
  for (const StepBar &Bar : Timeline.warmupBars()) {
    const Instrument &Instrument =
        Spec.Instruments.at(static_cast<std::size_t>(Bar.Instrument));
    Strategy.onBar(Bar.Instrument, Bar);
    (void)Strategy.evaluate(Bar.Instrument, Bar, Instrument.LotSize,
                            Instrument.PriceStep, Flat);
  }
}

} // namespace detail

RunOutput runBacktest(const SessionSpec &Spec,
                      std::shared_ptr<const Timeline> Timeline,
                      const dsl::Program &Program,
                      const std::vector<dsl::ResolvedParam> &Params,
                      const dsl::ParamValues &Overrides, dsl::Level StaticLevel,
                      const Progress &OnProgress) {
  return detail::runBacktestWithHook(Spec, std::move(Timeline), Program, Params,
                                     Overrides, StaticLevel, OnProgress, {});
}

} // namespace core
