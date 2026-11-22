#include "core/strategy/strategy_runner.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/report/metrics_input.hpp"
#include "core/strategy/price_bridge.hpp"
#include "core/strategy/runtime_detectors.hpp"
#include "dsl/interpreter.hpp"

namespace core {
namespace {

dsl::Bar toDslBar(const StepBar &BarItem) {
  dsl::Bar Out;
  Out.Step = BarItem.Step;
  Out.Open = BarItem.Open.toDouble();
  Out.High = BarItem.High.toDouble();
  Out.Low = BarItem.Low.toDouble();
  Out.Close = BarItem.Close.toDouble();
  Out.Volume = static_cast<double>(BarItem.VolumeLots);
  return Out;
}

OrderSpec toOrderSpec(std::size_t InstrIdx, const dsl::RuleIntent &Intent,
                      const Decimal &PriceStep) {
  OrderSpec OutSpec;
  OutSpec.Instrument = static_cast<int>(InstrIdx);
  OutSpec.OrderSide =
      (Intent.Kind == dsl::ActionKind::Buy) ? Side::Buy : Side::Sell;
  OutSpec.Lots = Intent.Lots;
  OutSpec.Tif = (Intent.Tif == dsl::TimeInForce::Day) ? TimeInForce::Day
                                                      : TimeInForce::Gtc;
  OutSpec.IsBlindPrice = false;
  switch (Intent.OrderKind) {
  case dsl::OrderType::Market:
    OutSpec.Type = OrderType::Market;
    break;
  case dsl::OrderType::Limit:
    OutSpec.Type = OrderType::Limit;
    OutSpec.LimitPrice = roundDslPriceToStep(Intent.RawPrice, PriceStep);
    break;
  case dsl::OrderType::Stop:
    OutSpec.Type = OrderType::Stop;
    OutSpec.StopPrice = roundDslPriceToStep(Intent.RawPrice, PriceStep);
    break;
  case dsl::OrderType::TakeProfit:
    OutSpec.Type = OrderType::TakeProfit;
    OutSpec.LimitPrice = roundDslPriceToStep(Intent.RawPrice, PriceStep);
    break;
  }
  return OutSpec;
}

const Order *findActiveOrder(const std::vector<Order> &Orders,
                             std::int64_t OrderId) {
  const auto It = std::lower_bound(
      Orders.begin(), Orders.end(), OrderId,
      [](const Order &Ord, std::int64_t Value) { return Ord.Id < Value; });
  if (It == Orders.end() || It->Id != OrderId) {
    return nullptr;
  }
  if (It->Status != OrderStatus::Active &&
      It->Status != OrderStatus::PartiallyFilled) {
    return nullptr;
  }
  return &*It;
}

bool checkCancelled(dsl::StrategyInstance &Instance,
                    const StrategyRunOptions &Opts,
                    std::chrono::steady_clock::time_point StartTime,
                    StrategyRunResult &Result) {
  if (Opts.ShouldCancel && Opts.ShouldCancel()) {
    Instance.requestCancel();
    Result.Cancelled = true;
    return true;
  }
  if (Opts.TimeBudget.has_value() &&
      (std::chrono::steady_clock::now() - StartTime) >= *Opts.TimeBudget) {
    Instance.requestCancel();
    Result.Cancelled = true;
    return true;
  }
  return false;
}

void recordIntegrityError(StrategyRunResult &Result, const std::string &Message,
                          std::int64_t Step) {
  Result.Invalid = true;
  Result.RuntimeFindings.push_back(
      Finding{"E-RT01", CheckLevel::Error, Message, Step});
  Result.FinalLevel = combineCheckLevel(Result.FinalLevel, CheckLevel::Error);
}

void handleCancelIntent(std::size_t InstrIdx, std::int64_t Step,
                        const dsl::RuleIntent &Intent, SessionEngine &Engine,
                        std::map<std::size_t, std::int64_t> &InstrActiveOrders,
                        std::int64_t &NextSeq, StrategyRunResult &Result) {
  (void)InstrIdx;
  std::vector<std::int64_t> ActiveIds;
  for (const auto &[RuleId, OrderId] : InstrActiveOrders) {
    (void)RuleId;
    if (findActiveOrder(Engine.orders(), OrderId) != nullptr) {
      ActiveIds.push_back(OrderId);
    }
  }
  InstrActiveOrders.clear();

  std::sort(ActiveIds.begin(), ActiveIds.end());
  ActiveIds.erase(std::unique(ActiveIds.begin(), ActiveIds.end()),
                  ActiveIds.end());
  if (ActiveIds.empty()) {
    return;
  }

  for (const std::int64_t OrderId : ActiveIds) {
    JournalEntry Entry;
    Entry.Seq = NextSeq++;
    Entry.Step = Step;
    Entry.Command = CancelCmd{OrderId, Reason::CancelledByUser};
    Entry.RuleName = Intent.RuleName;
    Entry.Because = Intent.Because;
    const CommandResult CmdRes = Engine.apply(Entry);
    Result.Journal.push_back(std::move(Entry));
    Result.CommandResults.push_back(CmdRes);
  }
}

void handleOrderIntent(std::size_t InstrIdx, std::int64_t Step,
                       const dsl::RuleIntent &Intent, const Decimal &PriceStep,
                       SessionEngine &Engine,
                       std::map<std::size_t, std::int64_t> &InstrActiveOrders,
                       std::int64_t &NextSeq, StrategyRunResult &Result) {
  const OrderSpec NewSpec = toOrderSpec(InstrIdx, Intent, PriceStep);

  const Order *Existing = nullptr;
  if (const auto It = InstrActiveOrders.find(Intent.RuleId);
      It != InstrActiveOrders.end()) {
    Existing = findActiveOrder(Engine.orders(), It->second);
    if (Existing == nullptr) {
      InstrActiveOrders.erase(It);
    }
  }

  if (Existing != nullptr) {
    if (Existing->Spec == NewSpec) {
      return;
    }
    JournalEntry Entry;
    Entry.Seq = NextSeq++;
    Entry.Step = Step;
    Entry.Command = ReplaceCmd{Existing->Id, PlaceCmd{NewSpec, std::nullopt}};
    Entry.RuleName = Intent.RuleName;
    Entry.Because = Intent.Because;
    const CommandResult CmdRes = Engine.apply(Entry);
    Result.Journal.push_back(std::move(Entry));
    Result.CommandResults.push_back(CmdRes);
    if (CmdRes.Accepted && !CmdRes.OrderIds.empty()) {
      InstrActiveOrders[Intent.RuleId] = CmdRes.OrderIds[0];
    }
    return;
  }

  JournalEntry Entry;
  Entry.Seq = NextSeq++;
  Entry.Step = Step;
  Entry.Command = PlaceCmd{NewSpec, std::nullopt};
  Entry.RuleName = Intent.RuleName;
  Entry.Because = Intent.Because;
  const CommandResult CmdRes = Engine.apply(Entry);
  Result.Journal.push_back(std::move(Entry));
  Result.CommandResults.push_back(CmdRes);
  if (CmdRes.Accepted && !CmdRes.OrderIds.empty()) {
    InstrActiveOrders[Intent.RuleId] = CmdRes.OrderIds[0];
  }
}

} // namespace

StrategyRunResult runStrategy(const SessionSpec &Spec,
                              std::shared_ptr<const Timeline> Tl,
                              const StrategyRunOptions &Opts) {
  if (!Tl) {
    throw std::invalid_argument("runStrategy: шкала времени не задана");
  }

  StrategyRunResult Result;
  const dsl::CompileResult CompileRes =
      dsl::compile(Opts.Source, Opts.ParamOverrides, Spec.Instruments.size());
  Result.Diagnostics = CompileRes.Diagnostics;
  Result.StaticLookahead = staticLookaheadLevel(Result.Diagnostics);
  Result.FinalLevel = Result.StaticLookahead;

  if (!CompileRes.Ok || !CompileRes.Instance) {
    Result.Compiled = false;
    return Result;
  }

  Result.Compiled = true;
  Result.Engine.emplace(Spec, Tl);
  SessionEngine &Engine = *Result.Engine;
  dsl::StrategyInstance &Instance = *CompileRes.Instance;

  const auto StartTime = std::chrono::steady_clock::now();
  const std::size_t NumInstruments = Spec.Instruments.size();
  std::vector<std::optional<std::int64_t>> LastBarStep(NumInstruments);

  for (const StepBar &WarmBar : Tl->warmupBars()) {
    if (checkCancelled(Instance, Opts, StartTime, Result)) {
      return Result;
    }
    const std::size_t InstrIdx = static_cast<std::size_t>(WarmBar.Instrument);
    if (InstrIdx >= NumInstruments ||
        (LastBarStep[InstrIdx].has_value() &&
         WarmBar.Step <= *LastBarStep[InstrIdx])) {
      recordIntegrityError(Result, "невозрастающий шаг бара на прогреве",
                           WarmBar.Step);
      return Result;
    }
    LastBarStep[InstrIdx] = WarmBar.Step;
    const dsl::StepOutcome Outcome = Instance.step(
        InstrIdx, toDslBar(WarmBar), dsl::PortfolioSnapshot{}, true);
    if (Outcome.Error.has_value()) {
      recordIntegrityError(Result, Outcome.Error->Message, WarmBar.Step);
      return Result;
    }
  }

  std::vector<std::map<std::size_t, std::int64_t>> ActiveOrderIds(
      NumInstruments);
  std::int64_t NextSeq = 0;

  while (Engine.status() == SessionStatus::Running) {
    if (checkCancelled(Instance, Opts, StartTime, Result)) {
      return Result;
    }

    const std::int64_t Step = Engine.currentStep();
    const PortfolioSnapshot Snap = Engine.snapshot();

    for (std::size_t InstrIdx = 0; InstrIdx < NumInstruments; ++InstrIdx) {
      if (checkCancelled(Instance, Opts, StartTime, Result)) {
        return Result;
      }
      const StepBar *BarPtr = Tl->barOf(static_cast<int>(InstrIdx), Step);
      if (BarPtr == nullptr) {
        continue;
      }
      if (BarPtr->Step != Step || (LastBarStep[InstrIdx].has_value() &&
                                   BarPtr->Step <= *LastBarStep[InstrIdx])) {
        recordIntegrityError(Result, "некорректный или невозрастающий шаг бара",
                             Step);
        return Result;
      }
      LastBarStep[InstrIdx] = BarPtr->Step;

      dsl::PortfolioSnapshot DslSnap;
      DslSnap.Position = Snap.Lots[InstrIdx];
      DslSnap.EntryPrice = (Snap.Lots[InstrIdx] == 0)
                               ? dsl::kNa
                               : Snap.AvgPrice[InstrIdx].toDouble();
      DslSnap.Cash = Snap.Cash.toDouble();
      DslSnap.Equity = Snap.Equity.toDouble();
      DslSnap.BarsInPosition = Snap.BarsInPosition[InstrIdx];
      DslSnap.LotSize = Spec.Instruments[InstrIdx].LotSize;
      DslSnap.PositionReopenedOnBar = Snap.PositionReopenedOnBar[InstrIdx];

      const dsl::StepOutcome Outcome =
          Instance.step(InstrIdx, toDslBar(*BarPtr), DslSnap, false);
      if (Outcome.Error.has_value()) {
        recordIntegrityError(Result, Outcome.Error->Message, Step);
        return Result;
      }

      const Decimal &PriceStep = Spec.Instruments[InstrIdx].PriceStep;
      for (const dsl::RuleIntent &Intent : Outcome.Intents) {
        if (Intent.Kind == dsl::ActionKind::Cancel) {
          handleCancelIntent(InstrIdx, Step, Intent, Engine,
                             ActiveOrderIds[InstrIdx], NextSeq, Result);
        } else {
          handleOrderIntent(InstrIdx, Step, Intent, PriceStep, Engine,
                            ActiveOrderIds[InstrIdx], NextSeq, Result);
        }
      }
    }

    if (Step < Tl->totalSteps() - 1 &&
        Engine.status() == SessionStatus::Running) {
      try {
        Engine.advance();
      } catch (const IntegrityViolation &E) {
        recordIntegrityError(Result, E.what(), Engine.currentStep());
        return Result;
      }
    } else {
      break;
    }
  }

  if (!Result.Invalid) {
    if (const auto GapsFinding =
            detectGuessedGaps(*Tl, Engine.orders(), Engine.fills())) {
      Result.RuntimeFindings.push_back(*GapsFinding);
      Result.FinalLevel =
          combineCheckLevel(Result.FinalLevel, GapsFinding->Severity);
    }
    if (const auto SmoothFinding =
            detectTooSmoothResult(Engine.portfolio().roundTrips())) {
      Result.RuntimeFindings.push_back(*SmoothFinding);
      Result.FinalLevel =
          combineCheckLevel(Result.FinalLevel, SmoothFinding->Severity);
    }
  }

  return Result;
}

Report buildStrategyReport(const StrategyRunResult &RunResult,
                           const Decimal &InitialCash, const Timeline &Tl,
                           const std::optional<BenchmarkSeries> &Benchmark,
                           const std::optional<RuoniaSeries> &Ruonia,
                           std::int32_t RuoniaPreviousDayYmd) {
  if (!RunResult.Engine.has_value()) {
    Report Out;
    Out.FinalEquity = InitialCash;
    Out.Findings = RunResult.RuntimeFindings;
    Out.ReportLevel = RunResult.FinalLevel;
    return Out;
  }

  const MetricsInput Input =
      buildMetricsInput(*RunResult.Engine, InitialCash, Tl, Benchmark, Ruonia,
                        RuoniaPreviousDayYmd);
  Report Out = computeReport(Input);
  std::vector<Finding> Extra;
  for (const Finding &Item : RunResult.RuntimeFindings) {
    if (Item.Code != "W-RT01" && Item.Code != "W-RT02") {
      Extra.push_back(Item);
    }
  }
  applyRuntimeChecks(Out, RunResult.StaticLookahead, Tl,
                     RunResult.Engine->orders(), RunResult.Engine->fills(),
                     Extra);
  return Out;
}

} // namespace core
