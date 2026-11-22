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
#include <vector>

#include "core/engine/digest.hpp"
#include "core/exec/costs.hpp"
#include "core/exec/execution.hpp"
#include "core/money/rounding.hpp"
#include "core/report/metrics_input.hpp"
#include "core/strategy/price_bridge.hpp"
#include "core/strategy/runtime_detectors.hpp"
#include "dsl/interpreter.hpp"

namespace core {
namespace {

constexpr std::int64_t kWindow = 3;
constexpr std::size_t kMaxAlgorithms = 5;
constexpr std::size_t kNone = static_cast<std::size_t>(-1);
const char *const kExcessReason = "алгоритм здесь не действовал бы";

bool isAction(const OrderSpec &Spec) {
  return Spec.Type == OrderType::Market || Spec.Type == OrderType::Limit;
}

std::int64_t signedLots(const OrderSpec &Spec) {
  return Spec.OrderSide == Side::Buy ? Spec.Lots : -Spec.Lots;
}

int signOf(std::int64_t Value) { return Value > 0 ? 1 : (Value < 0 ? -1 : 0); }

Int128 abs128(Int128 Value) { return Value < 0 ? -Value : Value; }

bool sizeDiffers(std::int64_t HumanLots, std::int64_t AlgoLots) {
  const Int128 Diff =
      abs128(static_cast<Int128>(HumanLots) - static_cast<Int128>(AlgoLots));
  const Int128 MaxAbs = std::max(abs128(static_cast<Int128>(HumanLots)),
                                 abs128(static_cast<Int128>(AlgoLots)));
  return Diff * 5 > MaxAbs;
}

bool isActive(const Order &Ord) {
  return Ord.Status == OrderStatus::Active ||
         Ord.Status == OrderStatus::PartiallyFilled;
}

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

dsl::PortfolioSnapshot toDslSnapshot(const PortfolioSnapshot &Snap,
                                     std::size_t InstrIdx,
                                     std::int64_t LotSize) {
  dsl::PortfolioSnapshot Out;
  Out.Position = Snap.Lots[InstrIdx];
  Out.EntryPrice = (Snap.Lots[InstrIdx] == 0)
                       ? dsl::kNa
                       : Snap.AvgPrice[InstrIdx].toDouble();
  Out.Cash = Snap.Cash.toDouble();
  Out.Equity = Snap.Equity.toDouble();
  Out.BarsInPosition = Snap.BarsInPosition[InstrIdx];
  Out.LotSize = LotSize;
  Out.PositionReopenedOnBar = Snap.PositionReopenedOnBar[InstrIdx];
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

using ActionKey = std::pair<int, std::int64_t>; // (Instrument, Step)

struct Action {
  int Instrument = 0;
  std::int64_t Step = 0;
  std::int64_t Lots = 0;
  std::vector<OrderSpec> Specs;
  std::vector<std::int64_t> OrderIds;
  std::string ReasonCode;
  std::optional<OrderType> PredominantType;
};

using ActionMap = std::map<ActionKey, Action>;

Action &actionAt(ActionMap &Map, int Instrument, std::int64_t Step) {
  Action &Item = Map[ActionKey{Instrument, Step}];
  Item.Instrument = Instrument;
  Item.Step = Step;
  return Item;
}

void finalizeHumanActions(ActionMap &Map,
                          const std::map<std::int64_t, OrderType> &OrderTypes) {
  for (auto It = Map.begin(); It != Map.end();) {
    if (It->second.Lots == 0) {
      It = Map.erase(It);
      continue;
    }
    bool AllMarket = !It->second.OrderIds.empty();
    bool AllLimit = !It->second.OrderIds.empty();
    for (const std::int64_t Id : It->second.OrderIds) {
      const OrderType Type = OrderTypes.at(Id);
      if (Type != OrderType::Market) {
        AllMarket = false;
      }
      if (Type != OrderType::Limit) {
        AllLimit = false;
      }
    }
    if (AllMarket) {
      It->second.PredominantType = OrderType::Market;
    } else if (AllLimit) {
      It->second.PredominantType = OrderType::Limit;
    } else {
      It->second.PredominantType = std::nullopt;
    }
    ++It;
  }
}

struct VirtualOrder {
  std::size_t RuleId = 0;
  OrderSpec Spec;
  std::int64_t PlacedStep = 0;
  std::int64_t RemainingLots = 0;
};

struct AlgoCandidate {
  std::size_t RuleId = 0;
  OrderSpec Spec;
  std::string Because;
};

using DedupKey = std::tuple<int, int, std::int64_t, std::optional<Decimal>,
                            int>; // (Side, Type, Lots, LimitPrice, Tif)

DedupKey dedupKeyOf(const OrderSpec &Spec) {
  return DedupKey{static_cast<int>(Spec.OrderSide), static_cast<int>(Spec.Type),
                  Spec.Lots, Spec.LimitPrice, static_cast<int>(Spec.Tif)};
}

std::int64_t liquidityCap(const StepBar &Bar, const CostConfig &Costs) {
  if (!Costs.LiquidityShare.has_value() || Bar.VolumeLots <= 0) {
    return 0;
  }
  return static_cast<std::int64_t>(Costs.LiquidityShare->times(Bar.VolumeLots)
                                       .round(0, RoundingMode::FLOOR)
                                       .raw() /
                                   Decimal::kUnit);
}

bool humanAllowsMoreThanCap(const VirtualOrder &Ord, const StepBar &Bar,
                            bool AtOpen, std::int64_t Cap,
                            const SessionSpec &Spec,
                            const Portfolio &HumanPortfolio) {
  const int Instr = Ord.Spec.Instrument;
  if (Ord.Spec.OrderSide == Side::Sell) {
    return HumanPortfolio.lots(Instr) > Cap;
  }
  const Instrument &Inst = Spec.Instruments[static_cast<std::size_t>(Instr)];
  const std::int64_t TargetLots = Cap + 1;
  try {
    Decimal Price = *Ord.Spec.LimitPrice;
    if (AtOpen) {
      const Decimal Fill =
          fillPrice(Side::Buy, Bar.Open, TargetLots, Bar.VolumeLots, Spec.Costs,
                    Inst.PriceStep);
      Price = std::min(Price, Fill);
    }
    const Amounts Amts = amounts(Price, TargetLots, Inst.LotSize, Spec.Costs);
    return Amts.Value + Amts.Commission + Amts.Fee <= HumanPortfolio.cash();
  } catch (const DecimalOverflow &) {
    return false;
  }
}

template <class PriceFn>
std::int64_t executedVirtualLots(const VirtualOrder &Ord, const StepBar &Bar,
                                 std::int64_t Cap, const SessionSpec &Spec,
                                 const Portfolio &HumanPortfolio,
                                 PriceFn &&PriceAt) {
  (void)Bar;
  const int Instr = Ord.Spec.Instrument;
  std::int64_t Q = std::min(Ord.RemainingLots, std::max<std::int64_t>(Cap, 0));
  if (Ord.Spec.OrderSide == Side::Sell) {
    return std::max<std::int64_t>(std::min(Q, HumanPortfolio.lots(Instr)), 0);
  }
  const Instrument &Inst = Spec.Instruments[static_cast<std::size_t>(Instr)];
  auto Fits = [&](std::int64_t Lots) {
    if (Lots <= 0) {
      return true;
    }
    try {
      const Decimal Price = PriceAt(Lots);
      const Amounts Amts = amounts(Price, Lots, Inst.LotSize, Spec.Costs);
      return Amts.Value + Amts.Commission + Amts.Fee <= HumanPortfolio.cash();
    } catch (const DecimalOverflow &) {
      return false;
    }
  };
  if (Q > 0 && !Fits(Q)) {
    std::int64_t Lo = 0;
    std::int64_t Hi = Q - 1;
    while (Lo < Hi) {
      const std::int64_t Mid = Lo + (Hi - Lo + 1) / 2;
      if (Fits(Mid)) {
        Lo = Mid;
      } else {
        Hi = Mid - 1;
      }
    }
    Q = Lo;
  }
  return Q;
}

void updateVirtualOrders(std::vector<VirtualOrder> &Orders, int Instr,
                         std::int64_t Step, const Timeline &Tl,
                         const SessionSpec &Spec,
                         const Portfolio &HumanPortfolio,
                         bool PositionReopenedOnBar) {
  const StepBar *Bar = Tl.barOf(Instr, Step);
  std::vector<char> Removed(Orders.size(), 0);

  if (Bar != nullptr) {
    std::optional<std::int64_t> RemainingCap;
    if (Spec.Costs.LiquidityShare.has_value()) {
      RemainingCap = liquidityCap(*Bar, Spec.Costs);
    }
    const Decimal &PriceStep =
        Spec.Instruments[static_cast<std::size_t>(Instr)].PriceStep;

    // Фаза 1: рыночные заявки.
    for (std::size_t I = 0; I < Orders.size(); ++I) {
      VirtualOrder &Ord = Orders[I];
      if (Ord.PlacedStep >= Step || Ord.Spec.Type != OrderType::Market) {
        continue;
      }
      if (RemainingCap.has_value()) {
        const std::int64_t Q = executedVirtualLots(
            Ord, *Bar, *RemainingCap, Spec, HumanPortfolio,
            [&](std::int64_t Lots) {
              return fillPrice(Ord.Spec.OrderSide, Bar->Open, Lots,
                               Bar->VolumeLots, Spec.Costs, PriceStep);
            });
        *RemainingCap -= Q;
      }
      Removed[I] = 1;
    }

    // Фаза 2: стоп-заявки.
    for (std::size_t I = 0; I < Orders.size(); ++I) {
      VirtualOrder &Ord = Orders[I];
      if (Ord.PlacedStep >= Step || Ord.Spec.Type != OrderType::Stop) {
        continue;
      }
      const Decimal &Stop = *Ord.Spec.StopPrice;
      const bool Buy = (Ord.Spec.OrderSide == Side::Buy);
      const bool Triggered = Buy ? (Bar->Open >= Stop || Bar->High >= Stop)
                                 : (Bar->Open <= Stop || Bar->Low <= Stop);
      if (!Triggered) {
        continue;
      }
      if (RemainingCap.has_value()) {
        const Decimal Base =
            Buy ? std::max(Bar->Open, Stop) : std::min(Bar->Open, Stop);
        const std::int64_t Q = executedVirtualLots(
            Ord, *Bar, *RemainingCap, Spec, HumanPortfolio,
            [&](std::int64_t Lots) {
              return fillPrice(Ord.Spec.OrderSide, Base, Lots, Bar->VolumeLots,
                               Spec.Costs, PriceStep);
            });
        *RemainingCap -= Q;
      }
      Removed[I] = 1;
    }

    // Фаза 3: лимитные и тейк-профит заявки.
    for (std::size_t I = 0; I < Orders.size(); ++I) {
      VirtualOrder &Ord = Orders[I];
      if (Ord.PlacedStep >= Step) {
        continue;
      }
      if (Ord.Spec.Type != OrderType::Limit &&
          Ord.Spec.Type != OrderType::TakeProfit) {
        continue;
      }
      const Decimal &Limit = *Ord.Spec.LimitPrice;
      const bool Buy = (Ord.Spec.OrderSide == Side::Buy);
      const bool AtOpen = Buy ? (Bar->Open <= Limit) : (Bar->Open >= Limit);
      const bool Crossed =
          AtOpen || (Buy ? (Bar->Low < Limit) : (Bar->High > Limit));
      if (!Crossed) {
        continue;
      }
      if (RemainingCap.has_value()) {
        const std::int64_t Cap = *RemainingCap;
        const std::int64_t Q = executedVirtualLots(
            Ord, *Bar, Cap, Spec, HumanPortfolio, [&](std::int64_t Lots) {
              if (!AtOpen) {
                return Limit;
              }
              const Decimal FilledPrice =
                  fillPrice(Ord.Spec.OrderSide, Bar->Open, Lots,
                            Bar->VolumeLots, Spec.Costs, PriceStep);
              return Buy ? std::min(Limit, FilledPrice)
                         : std::max(Limit, FilledPrice);
            });
        *RemainingCap -= Q;
        if (Cap < Ord.RemainingLots && Q == Cap &&
            humanAllowsMoreThanCap(Ord, *Bar, AtOpen, Cap, Spec,
                                   HumanPortfolio)) {
          Ord.RemainingLots -= Cap;
        } else {
          Removed[I] = 1;
        }
      } else {
        Removed[I] = 1;
      }
    }
  }

  std::vector<VirtualOrder> Kept;
  Kept.reserve(Orders.size());
  for (std::size_t I = 0; I < Orders.size(); ++I) {
    VirtualOrder &Ord = Orders[I];
    if (Ord.PlacedStep >= Step) {
      Kept.push_back(std::move(Ord));
      continue;
    }
    if (Removed[I]) {
      continue;
    }
    if (Tl.isLastStepOfDay(Step) && Ord.Spec.Tif == TimeInForce::Day &&
        Ord.PlacedStep + 1 < Tl.totalSteps() &&
        Tl.tradingDayOf(Ord.PlacedStep + 1) <= Tl.tradingDayOf(Step)) {
      continue;
    }
    if ((HumanPortfolio.lots(Instr) == 0 || PositionReopenedOnBar) &&
        Ord.Spec.OrderSide == Side::Sell) {
      continue;
    }
    Kept.push_back(std::move(Ord));
  }
  Orders = std::move(Kept);
}

void processStepIntents(std::size_t InstrIdx, std::int64_t Step,
                        const std::vector<dsl::RuleIntent> &Intents,
                        const Decimal &PriceStep,
                        const ExecutionEngine &ExecEngine,
                        const Portfolio &HumanPortfolio, const Timeline &Tl,
                        std::vector<VirtualOrder> &Virtuals,
                        ActionMap &OutAlgoActions) {
  std::vector<AlgoCandidate> Candidates;
  for (const dsl::RuleIntent &Intent : Intents) {
    if (Intent.Kind == dsl::ActionKind::Cancel) {
      Virtuals.clear();
      Candidates.clear();
      continue;
    }

    const OrderSpec NewSpec = toOrderSpec(InstrIdx, Intent, PriceStep);
    auto ExistingIt = std::find_if(
        Virtuals.begin(), Virtuals.end(),
        [&](const VirtualOrder &V) { return V.RuleId == Intent.RuleId; });
    if (ExistingIt != Virtuals.end() && ExistingIt->Spec == NewSpec) {
      continue;
    }

    const Validation Val =
        ExecEngine.validate(NewSpec, HumanPortfolio, Tl, Step);
    if (!Val.Ok) {
      continue;
    }

    if (ExistingIt != Virtuals.end()) {
      Virtuals.erase(ExistingIt);
    }
    Virtuals.push_back(
        VirtualOrder{Intent.RuleId, NewSpec, Step, NewSpec.Lots});

    if (NewSpec.Type == OrderType::Market || NewSpec.Type == OrderType::Limit) {
      Candidates.erase(std::remove_if(Candidates.begin(), Candidates.end(),
                                      [&](const AlgoCandidate &C) {
                                        return C.RuleId == Intent.RuleId;
                                      }),
                       Candidates.end());
      Candidates.push_back(
          AlgoCandidate{Intent.RuleId, NewSpec, Intent.Because});
    }
  }

  std::set<DedupKey> SeenKeys;
  std::vector<AlgoCandidate> Collapsed;
  for (const AlgoCandidate &Cand : Candidates) {
    if (SeenKeys.insert(dedupKeyOf(Cand.Spec)).second) {
      Collapsed.push_back(Cand);
    }
  }
  if (Collapsed.empty()) {
    return;
  }

  std::int64_t NetLots = 0;
  for (const AlgoCandidate &Cand : Collapsed) {
    NetLots += signedLots(Cand.Spec);
  }
  if (NetLots == 0) {
    return;
  }

  Action &Act = actionAt(OutAlgoActions, static_cast<int>(InstrIdx), Step);
  Act.Lots = NetLots;
  bool AllMarket = true;
  bool AllLimit = true;
  for (const AlgoCandidate &Cand : Collapsed) {
    Act.Specs.push_back(Cand.Spec);
    if (Cand.Spec.Type != OrderType::Market) {
      AllMarket = false;
    }
    if (Cand.Spec.Type != OrderType::Limit) {
      AllLimit = false;
    }
    if (!Cand.Because.empty()) {
      if (!Act.ReasonCode.empty()) {
        Act.ReasonCode += "; ";
      }
      Act.ReasonCode += Cand.Because;
    }
  }
  if (AllMarket) {
    Act.PredominantType = OrderType::Market;
  } else if (AllLimit) {
    Act.PredominantType = OrderType::Limit;
  } else {
    Act.PredominantType = std::nullopt;
  }
}

Decimal actionValue(const std::vector<Fill> &Fills,
                    const std::set<std::int64_t> &OrderIds,
                    std::int64_t HorizonStep, const Timeline &Tl,
                    const SessionSpec &Spec) {
  Decimal Value;
  for (const Fill &Item : Fills) {
    if (Item.Step > HorizonStep || OrderIds.count(Item.OrderId) == 0) {
      continue;
    }
    const StepBar *Bar = Tl.lastBarOf(Item.Instrument, HorizonStep);
    if (Bar == nullptr) {
      continue;
    }
    const std::int64_t Units =
        Item.Lots *
        Spec.Instruments[static_cast<std::size_t>(Item.Instrument)].LotSize;
    Decimal Gain = (Bar->Close - Item.Price).times(Units);
    if (Item.OrderSide == Side::Sell) {
      Gain = -Gain;
    }
    Value += Gain - Item.Commission - Item.Fee;
  }
  return Value;
}

template <class StepFn, class EntryFn>
std::optional<SessionEngine>
walkHuman(const SessionSpec &Spec, std::shared_ptr<const Timeline> Tl,
          std::span<const JournalEntry> Journal, std::int64_t LastStep,
          StepFn &&OnStep, EntryFn &&OnEntry,
          const std::function<bool()> &ShouldCancel = {}) {
  SessionEngine Engine(Spec, std::move(Tl));
  std::size_t Next = 0;
  for (;;) {
    if (ShouldCancel && ShouldCancel()) {
      return std::nullopt;
    }
    const std::int64_t K = Engine.currentStep();
    const bool ActiveStep =
        (Engine.status() == SessionStatus::Running && K < LastStep);
    if (ActiveStep) {
      OnStep(K, Engine);
      if (ShouldCancel && ShouldCancel()) {
        return std::nullopt;
      }
    }
    while (Next < Journal.size() &&
           (Journal[Next].Step == K ||
            Engine.status() != SessionStatus::Running)) {
      const CommandResult Res = Engine.apply(Journal[Next]);
      OnEntry(Journal[Next], Res, ActiveStep);
      ++Next;
    }
    if (!ActiveStep) {
      break;
    }
    Engine.advance();
  }
  return Engine;
}

Decimal medianOf(std::vector<Decimal> Costs) {
  if (Costs.empty()) {
    return Decimal::zero();
  }
  std::sort(Costs.begin(), Costs.end());
  const std::size_t N = Costs.size();
  if (N % 2 == 1) {
    return Costs[N / 2];
  }
  return (Costs[N / 2 - 1] + Costs[N / 2])
      .div(Decimal::fromUnits(2), 2, RoundingMode::HalfUp);
}

std::optional<BenchmarkSeries>
clipBenchmark(const std::optional<BenchmarkSeries> &Benchmark,
              const Timeline &Tl, std::int64_t Step) {
  if (!Benchmark.has_value()) {
    return std::nullopt;
  }
  const std::optional<std::int32_t> MaxYmd = lastCompletedDayYmd(Tl, Step);
  if (!MaxYmd.has_value()) {
    return std::nullopt;
  }
  BenchmarkSeries Out;
  Out.FirstOpen = Benchmark->FirstOpen;
  for (const DailyClose &Item : Benchmark->Closes) {
    if (Item.Yyyymmdd <= *MaxYmd) {
      Out.Closes.push_back(Item);
    }
  }
  if (Out.Closes.empty()) {
    return std::nullopt;
  }
  return Out;
}

struct Pending {
  Divergence Result;
  std::size_t Algorithm = 0;
  const Action *Human = nullptr;
  std::int64_t SnapStep = -1;
  std::int64_t PercentStep = 0;
  std::vector<OrderSpec> AlgoSpecs;
  Decimal HumanValue;
  Decimal AlgoValue;
};

struct HumanOutcome {
  bool Matched = false;
  std::size_t PendingIndex = kNone;
};

class Analyzer {
public:
  Analyzer(const SessionSpec &SessionConfig, std::shared_ptr<const Timeline> Tl,
           std::span<const JournalEntry> Entries, std::int64_t UpToStep,
           const std::vector<AlgorithmInput> &Inputs,
           const std::optional<BenchmarkSeries> &BenchmarkIn,
           const std::optional<RuoniaSeries> &RuoniaIn,
           std::int32_t RuoniaPrevDayYmd, std::function<bool()> CancelCheck)
      : Spec(SessionConfig), TimelineRef(std::move(Tl)), Journal(Entries),
        LastStep(UpToStep), Algorithms(Inputs), Benchmark(BenchmarkIn),
        Ruonia(RuoniaIn), RuoniaPreviousDayYmd(RuoniaPrevDayYmd),
        ShouldCancel(std::move(CancelCheck)),
        ExecEngine(Spec.Instruments, Spec.Costs) {}

  DivergenceResult run() {
    DivergenceResult Result;
    validate();
    if (isCancelled()) {
      Result.Cancelled = true;
      return Result;
    }
    if (!collectActions() || isCancelled()) {
      Result.Cancelled = true;
      return Result;
    }
    for (std::size_t A = 0; A < Algorithms.size(); ++A) {
      if (isCancelled()) {
        Result.Cancelled = true;
        return Result;
      }
      match(A);
    }
    priceHumanSide();
    if (!priceAlgorithmSide() || isCancelled()) {
      Result.Cancelled = true;
      return Result;
    }
    const std::vector<std::int64_t> PendingToSeq = assembleDivergences(Result);
    assembleMissedTrades(Result, PendingToSeq);
    assembleConsensus(Result);
    assembleIndependent(Result);
    return Result;
  }

private:
  bool isCancelled() const { return ShouldCancel && ShouldCancel(); }
  void validate() {
    if (!TimelineRef) {
      throw std::invalid_argument("analyzeDivergences: нет шкалы времени");
    }
    if (LastStep < 0 || LastStep >= TimelineRef->totalSteps()) {
      throw std::invalid_argument(
          "analyzeDivergences: lastStep вне шкалы времени");
    }
    if (Algorithms.size() > kMaxAlgorithms) {
      throw std::invalid_argument("analyzeDivergences: алгоритмов больше " +
                                  std::to_string(kMaxAlgorithms));
    }
    std::set<std::string> Ids;
    for (const AlgorithmInput &Input : Algorithms) {
      if (!Ids.insert(Input.Id).second) {
        throw std::invalid_argument(
            "analyzeDivergences: повтор id алгоритма: " + Input.Id);
      }
    }
    std::int64_t PrevStep = 0;
    std::int64_t PrevSeq = -1;
    for (const JournalEntry &Entry : Journal) {
      if (Entry.Step < PrevStep) {
        throw std::invalid_argument(
            "analyzeDivergences: шаг записи журнала убывает");
      }
      if (Entry.Step > LastStep) {
        throw std::invalid_argument(
            "analyzeDivergences: шаг записи журнала больше lastStep");
      }
      if (Entry.Seq < 0 || Entry.Seq <= PrevSeq) {
        throw std::invalid_argument(
            "analyzeDivergences: seq записей журнала не возрастает");
      }
      PrevStep = Entry.Step;
      PrevSeq = Entry.Seq;
    }

    const std::size_t NumInstruments = Spec.Instruments.size();
    Instances.reserve(Algorithms.size());
    for (const AlgorithmInput &Input : Algorithms) {
      if (isCancelled()) {
        return;
      }
      dsl::CompileResult CompileRes =
          dsl::compile(Input.Source, Input.ParamOverrides, NumInstruments);
      if (!CompileRes.Ok || !CompileRes.Instance ||
          staticLookaheadLevel(CompileRes.Diagnostics) == CheckLevel::Error) {
        throw std::invalid_argument(
            "analyzeDivergences: стратегия не прошла проверку: " + Input.Id);
      }
      Instances.push_back(std::move(CompileRes.Instance));
    }
  }

  bool collectActions() {
    const std::size_t NumInstruments = Spec.Instruments.size();
    std::vector<std::optional<std::int64_t>> WarmupLastStep(NumInstruments);
    for (const StepBar &WarmBar : TimelineRef->warmupBars()) {
      if (isCancelled()) {
        return false;
      }
      const std::size_t InstrIdx = static_cast<std::size_t>(WarmBar.Instrument);
      if (InstrIdx >= NumInstruments ||
          (WarmupLastStep[InstrIdx].has_value() &&
           WarmBar.Step <= *WarmupLastStep[InstrIdx])) {
        throw IntegrityViolation(
            "analyzeDivergences: невозрастающий шаг бара на прогреве");
      }
      WarmupLastStep[InstrIdx] = WarmBar.Step;
      const dsl::Bar DslBar = toDslBar(WarmBar);
      for (const auto &InstPtr : Instances) {
        if (isCancelled()) {
          return false;
        }
        const dsl::StepOutcome Out =
            InstPtr->step(InstrIdx, DslBar, dsl::PortfolioSnapshot{}, true);
        if (Out.Error.has_value()) {
          throw IntegrityViolation(Out.Error->Message);
        }
      }
    }

    AlgoActions.assign(Algorithms.size(), {});
    std::vector<std::vector<std::vector<VirtualOrder>>> VirtualOrders(
        Algorithms.size(),
        std::vector<std::vector<VirtualOrder>>(NumInstruments));
    std::vector<std::optional<std::int64_t>> LastBarStep = WarmupLastStep;

    const auto OnStep = [&](std::int64_t K, const SessionEngine &Engine) {
      if (isCancelled()) {
        return;
      }
      const PortfolioSnapshot Snap = Engine.snapshot();
      for (std::size_t I = 0; I < NumInstruments; ++I) {
        if (isCancelled()) {
          return;
        }
        const int Instr = static_cast<int>(I);
        if (K >= 1) {
          for (std::size_t A = 0; A < Algorithms.size(); ++A) {
            updateVirtualOrders(VirtualOrders[A][I], Instr, K, *TimelineRef,
                                Spec, Engine.portfolio(),
                                Snap.PositionReopenedOnBar[I]);
          }
        }
        const StepBar *BarPtr = TimelineRef->barOf(Instr, K);
        if (BarPtr == nullptr) {
          continue;
        }
        if (BarPtr->Step != K ||
            (LastBarStep[I].has_value() && BarPtr->Step <= *LastBarStep[I])) {
          throw IntegrityViolation(
              "analyzeDivergences: некорректный или невозрастающий шаг бара");
        }
        LastBarStep[I] = BarPtr->Step;

        const dsl::Bar DslBar = toDslBar(*BarPtr);
        const dsl::PortfolioSnapshot DslSnap =
            toDslSnapshot(Snap, I, Spec.Instruments[I].LotSize);
        const Decimal &PriceStep = Spec.Instruments[I].PriceStep;

        for (std::size_t A = 0; A < Instances.size(); ++A) {
          if (isCancelled()) {
            return;
          }
          const dsl::StepOutcome Out =
              Instances[A]->step(I, DslBar, DslSnap, false);
          if (Out.Error.has_value()) {
            throw IntegrityViolation(Out.Error->Message);
          }
          processStepIntents(I, K, Out.Intents, PriceStep, ExecEngine,
                             Engine.portfolio(), *TimelineRef,
                             VirtualOrders[A][I], AlgoActions[A]);
        }
      }
    };

    const auto OnEntry = [&](const JournalEntry &Entry,
                             const CommandResult &Result, bool ActiveStep) {
      if (!ActiveStep || !Result.Accepted) {
        return;
      }
      const PlaceCmd *Place = std::get_if<PlaceCmd>(&Entry.Command);
      std::int64_t Gone = -1;
      if (const auto *Replace = std::get_if<ReplaceCmd>(&Entry.Command)) {
        Place = &Replace->Replacement;
        Gone = Replace->OrderId;
      } else if (const auto *Cancel = std::get_if<CancelCmd>(&Entry.Command)) {
        Gone = Cancel->OrderId;
      }
      if (Gone >= 0) {
        const auto Old = HumanOrderToAction.find(Gone);
        if (Old != HumanOrderToAction.end() &&
            Old->second.second == Entry.Step) {
          Action &Act = HumanActions.at(Old->second);
          Act.Lots -= HumanOrderLots.at(Gone);
          Act.OrderIds.erase(
              std::find(Act.OrderIds.begin(), Act.OrderIds.end(), Gone));
          HumanOrderLots.erase(Gone);
          HumanOrderTypes.erase(Gone);
          HumanOrderSides.erase(Gone);
          HumanOrderToAction.erase(Old);
        }
      }
      if (Place == nullptr || Result.OrderIds.empty() || Place->OcoWith ||
          !isAction(Place->Order)) {
        return;
      }
      const std::int64_t NewId = Result.OrderIds.front();
      Action &Act = actionAt(HumanActions, Place->Order.Instrument, Entry.Step);
      Act.Lots += signedLots(Place->Order);
      Act.OrderIds.push_back(NewId);
      HumanOrderToAction[NewId] =
          ActionKey{Place->Order.Instrument, Entry.Step};
      HumanOrderLots[NewId] = signedLots(Place->Order);
      HumanOrderTypes[NewId] = Place->Order.Type;
      HumanOrderSides[NewId] = Place->Order.OrderSide;
    };

    const auto CancelFn = [&]() { return isCancelled(); };
    auto Walked = walkHuman(Spec, TimelineRef, Journal, LastStep, OnStep,
                            OnEntry, CancelFn);
    if (!Walked.has_value()) {
      return false;
    }
    Human.emplace(std::move(*Walked));
    LastSession = Human->currentStep();
    finalizeHumanActions(HumanActions, HumanOrderTypes);
    for (auto It = HumanOrderToAction.begin();
         It != HumanOrderToAction.end();) {
      const auto SideIt = HumanOrderSides.find(It->first);
      const bool IsBuy =
          SideIt != HumanOrderSides.end() && SideIt->second == Side::Buy;
      It = (!IsBuy && HumanActions.count(It->second) == 0)
               ? HumanOrderToAction.erase(It)
               : std::next(It);
    }
    return true;
  }

  std::int64_t horizonAfter(std::size_t Algorithm, int Instrument,
                            std::int64_t At) const {
    std::optional<std::int64_t> Best;
    const auto Consider = [&](const ActionMap &Map) {
      const auto It = Map.upper_bound(ActionKey{Instrument, At});
      if (It != Map.end() && It->first.first == Instrument &&
          (!Best || It->first.second < *Best)) {
        Best = It->first.second;
      }
    };
    Consider(HumanActions);
    Consider(AlgoActions[Algorithm]);
    return Best ? *Best : LastSession;
  }

  void match(std::size_t A) {
    for (std::size_t I = 0; I < Spec.Instruments.size(); ++I) {
      const int Instrument = static_cast<int>(I);
      std::vector<const Action *> Hs;
      std::vector<const Action *> As;
      for (const auto &[Key, Act] : HumanActions) {
        if (Key.first == Instrument) {
          Hs.push_back(&Act);
        }
      }
      for (const auto &[Key, Act] : AlgoActions[A]) {
        if (Key.first == Instrument) {
          As.push_back(&Act);
        }
      }
      std::vector<char> HMatched(Hs.size(), 0);
      std::vector<char> AMatched(As.size(), 0);
      std::vector<std::size_t> HPending(Hs.size(), kNone);

      for (std::size_t H = 0; H < Hs.size(); ++H) {
        for (std::size_t K = 0; K < As.size(); ++K) {
          if (AMatched[K] || As[K]->Step != Hs[H]->Step ||
              signOf(As[K]->Lots) != signOf(Hs[H]->Lots)) {
            continue;
          }
          HMatched[H] = AMatched[K] = 1;
          MatchedAlgoActions.insert({A, Instrument, As[K]->Step});
          Outcome[{A, Hs[H]->Instrument, Hs[H]->Step}].Matched = true;
          if (sizeDiffers(Hs[H]->Lots, As[K]->Lots)) {
            HPending[H] =
                makePending(A, DivergenceType::Size, Hs[H], As[K], Hs[H]->Step);
          }
          break;
        }
      }

      for (std::size_t H = 0; H < Hs.size(); ++H) {
        if (HMatched[H]) {
          continue;
        }
        std::size_t Best = kNone;
        std::int64_t BestDistance = 0;
        for (std::size_t K = 0; K < As.size(); ++K) {
          if (AMatched[K] || signOf(As[K]->Lots) != signOf(Hs[H]->Lots)) {
            continue;
          }
          const std::int64_t Distance = As[K]->Step > Hs[H]->Step
                                            ? As[K]->Step - Hs[H]->Step
                                            : Hs[H]->Step - As[K]->Step;
          if (Distance < 1 || Distance > kWindow) {
            continue;
          }
          if (Best == kNone || Distance < BestDistance) {
            Best = K;
            BestDistance = Distance;
          }
        }
        if (Best == kNone) {
          continue;
        }
        HMatched[H] = AMatched[Best] = 1;
        MatchedAlgoActions.insert({A, Instrument, As[Best]->Step});
        Outcome[{A, Hs[H]->Instrument, Hs[H]->Step}].Matched = true;
        HPending[H] = makePending(A, DivergenceType::Timing, Hs[H], As[Best],
                                  std::max(Hs[H]->Step, As[Best]->Step));
      }

      for (std::size_t H = 0; H < Hs.size(); ++H) {
        if (HMatched[H]) {
          continue;
        }
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
        if (!AMatched[K]) {
          makePending(A, DivergenceType::Missed, nullptr, As[K], As[K]->Step);
        }
      }

      for (std::size_t H = 0; H < Hs.size(); ++H) {
        Outcome[{A, Hs[H]->Instrument, Hs[H]->Step}].PendingIndex = HPending[H];
      }
    }
  }

  std::size_t makePending(std::size_t A, DivergenceType Type,
                          const Action *HumanAct, const Action *Algo,
                          std::int64_t At) {
    Pending Item;
    Item.Algorithm = A;
    Item.Human = HumanAct;
    Divergence &D = Item.Result;
    D.AlgorithmId = Algorithms[A].Id;
    D.Instrument =
        HumanAct != nullptr ? HumanAct->Instrument : Algo->Instrument;
    D.Type = Type;
    if (HumanAct != nullptr) {
      D.HumanStep = HumanAct->Step;
      D.HumanLots = HumanAct->Lots;
      D.HumanOrderType = HumanAct->PredominantType;
    }
    if (Algo != nullptr) {
      D.AlgoStep = Algo->Step;
      D.AlgoLots = Algo->Lots;
      D.AlgoOrderType = Algo->PredominantType;
      Item.AlgoSpecs = Algo->Specs;
      Item.SnapStep = Algo->Step;
    }
    D.HorizonStep = horizonAfter(A, D.Instrument, At);
    Item.PercentStep = At;
    D.ReasonCode = Type == DivergenceType::Excess
                       ? kExcessReason
                       : (Algo != nullptr ? Algo->ReasonCode : "");
    PendingList.push_back(std::move(Item));
    return PendingList.size() - 1;
  }

  void priceHumanSide() {
    for (Pending &P : PendingList) {
      if (P.Human == nullptr) {
        continue;
      }
      const std::set<std::int64_t> Ids(P.Human->OrderIds.begin(),
                                       P.Human->OrderIds.end());
      P.HumanValue = actionValue(Human->fills(), Ids, P.Result.HorizonStep,
                                 *TimelineRef, Spec);
    }
  }

  bool priceAlgorithmSide() {
    std::map<std::int64_t, std::vector<std::size_t>> Jobs;
    for (std::size_t Index = 0; Index < PendingList.size(); ++Index) {
      if (PendingList[Index].SnapStep >= 0) {
        Jobs[PendingList[Index].SnapStep].push_back(Index);
      }
    }
    if (!Jobs.empty()) {
      const std::int64_t SeqBase =
          (Journal.empty() ? 0 : Journal.back().Seq) + 1;
      const auto OnStep = [&](std::int64_t K, const SessionEngine &Engine) {
        const auto Found = Jobs.find(K);
        if (Found == Jobs.end()) {
          return;
        }
        for (const std::size_t Index : Found->second) {
          if (isCancelled()) {
            return;
          }
          PendingList[Index].AlgoValue =
              algorithmValue(Engine, PendingList[Index], SeqBase);
        }
      };
      auto NoEntry = [](const JournalEntry &, const CommandResult &, bool) {};
      const auto CancelFn = [&]() { return isCancelled(); };
      const std::int64_t LastJob = Jobs.rbegin()->first + 1;
      auto Walked = walkHuman(Spec, TimelineRef, Journal, LastJob, OnStep,
                              NoEntry, CancelFn);
      if (!Walked.has_value()) {
        return false;
      }
    }
    if (isCancelled()) {
      return false;
    }
    const Decimal Hundred = Decimal::fromUnits(100);
    const auto &Equity = Human->equity();
    for (Pending &P : PendingList) {
      Divergence &D = P.Result;
      const Decimal ExactDiff = P.AlgoValue - P.HumanValue;
      D.Cost = ExactDiff.round(2, RoundingMode::HalfUp);
      const std::size_t At = static_cast<std::size_t>(P.PercentStep);
      const Decimal EquityAt =
          At < Equity.size() ? Equity[At].Equity : Decimal::zero();
      D.CostPercent =
          EquityAt > Decimal::zero()
              ? ExactDiff.mulDiv(Hundred, EquityAt, 2, RoundingMode::HalfUp)
              : Decimal::zero();
    }
    return true;
  }

  Decimal algorithmValue(const SessionEngine &At, const Pending &P,
                         std::int64_t SeqBase) const {
    SessionEngine Copy = At;
    std::int64_t Seq = SeqBase;
    const std::int64_t Step = Copy.currentStep();
    for (const Order &Ord : At.orders()) {
      if (!isActive(Ord)) {
        continue;
      }
      JournalEntry Cancel;
      Cancel.Seq = Seq++;
      Cancel.Step = Step;
      Cancel.Command = CancelCmd{Ord.Id, Reason::CancelledByUser};
      Copy.apply(Cancel);
    }
    std::set<std::int64_t> Ids;
    for (const OrderSpec &Leg : P.AlgoSpecs) {
      JournalEntry Entry;
      Entry.Seq = Seq++;
      Entry.Step = Step;
      Entry.Command = PlaceCmd{Leg, std::nullopt};
      const CommandResult Res = Copy.apply(Entry);
      Ids.insert(Res.OrderIds.begin(), Res.OrderIds.end());
    }
    while (!isCancelled() && Copy.currentStep() < P.Result.HorizonStep &&
           Copy.status() == SessionStatus::Running) {
      Copy.advance();
    }
    return actionValue(Copy.fills(), Ids, P.Result.HorizonStep, *TimelineRef,
                       Spec);
  }

  std::vector<std::int64_t>
  assembleDivergences(DivergenceResult &Result) const {
    std::vector<std::int64_t> PendingToSeq(PendingList.size(), -1);
    std::int64_t NextSeq = 0;
    const auto Primary = [](const Divergence &D) {
      return D.HumanStep >= 0 ? D.HumanStep : D.AlgoStep;
    };
    for (std::size_t A = 0; A < Algorithms.size(); ++A) {
      std::vector<std::size_t> Mine;
      for (std::size_t PIdx = 0; PIdx < PendingList.size(); ++PIdx) {
        if (PendingList[PIdx].Algorithm == A) {
          Mine.push_back(PIdx);
        }
      }
      std::stable_sort(Mine.begin(), Mine.end(),
                       [&](std::size_t XIdx, std::size_t YIdx) {
                         const Divergence &X = PendingList[XIdx].Result;
                         const Divergence &Y = PendingList[YIdx].Result;
                         return std::make_tuple(Primary(X), X.Instrument,
                                                static_cast<int>(X.Type)) <
                                std::make_tuple(Primary(Y), Y.Instrument,
                                                static_cast<int>(Y.Type));
                       });
      for (const std::size_t PIdx : Mine) {
        Divergence Div = PendingList[PIdx].Result;
        Div.Seq = NextSeq;
        PendingToSeq[PIdx] = NextSeq;
        ++NextSeq;
        Result.Divergences.push_back(std::move(Div));
      }
    }
    return PendingToSeq;
  }

  bool algoMadeBuyNear(std::size_t A, int Instrument, std::int64_t Step) const {
    const ActionMap &Map = AlgoActions[A];
    const auto Low = Map.lower_bound(ActionKey{Instrument, Step - kWindow});
    const auto High = Map.upper_bound(ActionKey{Instrument, Step + kWindow});
    for (auto It = Low; It != High; ++It) {
      if (It->second.Lots > 0 &&
          MatchedAlgoActions.count({A, Instrument, It->first.second}) == 0) {
        return true;
      }
    }
    return false;
  }

  void
  assembleMissedTrades(DivergenceResult &Result,
                       const std::vector<std::int64_t> &PendingToSeq) const {
    std::int64_t NextSeq = 0;
    for (const RoundTrip &Trip : Human->portfolio().roundTrips()) {
      if (Trip.NetPnl.sign() <= 0) {
        continue;
      }
      const auto Entry = HumanOrderToAction.find(Trip.EntryOrderId);
      if (Entry == HumanOrderToAction.end()) {
        continue;
      }
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
      const auto HumanIt = HumanActions.find(Entry->second);
      const bool HasHumanBuy =
          HumanIt != HumanActions.end() && HumanIt->second.Lots > 0;
      for (std::size_t A = 0; A < Algorithms.size(); ++A) {
        if (HasHumanBuy) {
          const auto Found =
              Outcome.find({A, Entry->second.first, Entry->second.second});
          if (Found != Outcome.end() && !Found->second.Matched) {
            Missed.AlgorithmIds.push_back(Algorithms[A].Id);
            if (Found->second.PendingIndex != kNone) {
              const Divergence &Div =
                  PendingList[Found->second.PendingIndex].Result;
              if (Div.Type == DivergenceType::Excess && Div.HumanLots > 0) {
                Missed.DivergenceSeqs.push_back(
                    PendingToSeq[Found->second.PendingIndex]);
              }
            }
          }
        } else if (!algoMadeBuyNear(A, Entry->second.first,
                                    Entry->second.second)) {
          Missed.AlgorithmIds.push_back(Algorithms[A].Id);
        }
      }
      if (!Missed.AlgorithmIds.empty()) {
        Missed.Seq = NextSeq++;
        Result.Missed.push_back(std::move(Missed));
      }
    }
  }

  void assembleConsensus(DivergenceResult &Result) const {
    const std::int32_t Total = static_cast<std::int32_t>(Algorithms.size());
    if (Total == 0) {
      return;
    }
    std::vector<ConsensusItem> HumanItems;
    for (const auto &[Key, Act] : HumanActions) {
      std::int32_t Agreeing = 0;
      std::vector<Decimal> Costs;
      for (std::size_t A = 0; A < Algorithms.size(); ++A) {
        const auto Found = Outcome.find({A, Key.first, Key.second});
        if (Found == Outcome.end()) {
          continue;
        }
        if (Found->second.Matched) {
          ++Agreeing;
        } else if (Found->second.PendingIndex != kNone) {
          Costs.push_back(PendingList[Found->second.PendingIndex].Result.Cost);
        }
      }
      if (Agreeing == Total) {
        continue;
      }
      HumanItems.push_back(ConsensusItem{
          Key.first, Key.second, Act.Lots > 0 ? Side::Buy : Side::Sell, true,
          Agreeing, Total, medianOf(Costs)});
    }
    std::sort(HumanItems.begin(), HumanItems.end(),
              [](const ConsensusItem &X, const ConsensusItem &Y) {
                return std::make_tuple(X.Step, X.Instrument) <
                       std::make_tuple(Y.Step, Y.Instrument);
              });

    std::map<std::tuple<std::int64_t, int, int>, std::vector<Decimal>> Missed;
    for (const Pending &P : PendingList) {
      const Divergence &D = P.Result;
      if (D.Type != DivergenceType::Missed) {
        continue;
      }
      Missed[{D.AlgoStep, D.Instrument, D.AlgoLots > 0 ? 0 : 1}].push_back(
          D.Cost);
    }

    Result.Consensus = std::move(HumanItems);
    for (const auto &[Key, Costs] : Missed) {
      Result.Consensus.push_back(ConsensusItem{
          std::get<1>(Key), std::get<0>(Key),
          std::get<2>(Key) == 0 ? Side::Buy : Side::Sell, false,
          static_cast<std::int32_t>(Costs.size()), Total, medianOf(Costs)});
    }
  }

  void assembleIndependent(DivergenceResult &Result) const {
    Result.Independent.reserve(Algorithms.size());
    for (const AlgorithmInput &Input : Algorithms) {
      if (isCancelled()) {
        Result.Cancelled = true;
        return;
      }
      StrategyRunOptions Opts;
      Opts.Source = Input.Source;
      Opts.ParamOverrides = Input.ParamOverrides;
      Opts.ShouldCancel = ShouldCancel;
      Opts.UpToStep = LastSession;
      StrategyRunResult Run = runStrategy(Spec, TimelineRef, Opts);
      if (Run.Cancelled || isCancelled()) {
        Result.Cancelled = true;
        return;
      }
      const std::int64_t EndStep =
          Run.Engine.has_value() ? Run.Engine->currentStep() : LastSession;
      const std::optional<BenchmarkSeries> ClippedBenchmark =
          clipBenchmark(Benchmark, *TimelineRef, EndStep);
      Report PortfolioReport =
          buildStrategyReport(Run, Spec.InitialCash, *TimelineRef,
                              ClippedBenchmark, Ruonia, RuoniaPreviousDayYmd);
      const std::uint64_t Digest =
          Run.Engine.has_value() ? digestOf(*Run.Engine) : 0;
      Result.Independent.push_back(AlgorithmPortfolio{
          std::move(Run), std::move(PortfolioReport), Digest});
    }
  }

  const SessionSpec &Spec;
  std::shared_ptr<const Timeline> TimelineRef;
  std::span<const JournalEntry> Journal;
  std::int64_t LastStep = 0;
  std::int64_t LastSession = 0;
  const std::vector<AlgorithmInput> &Algorithms;
  const std::optional<BenchmarkSeries> &Benchmark;
  const std::optional<RuoniaSeries> &Ruonia;
  std::int32_t RuoniaPreviousDayYmd = 0;
  std::function<bool()> ShouldCancel;
  ExecutionEngine ExecEngine;

  std::vector<std::unique_ptr<dsl::StrategyInstance>> Instances;
  std::optional<SessionEngine> Human;
  ActionMap HumanActions;
  std::map<std::int64_t, ActionKey> HumanOrderToAction;
  std::map<std::int64_t, std::int64_t> HumanOrderLots;
  std::map<std::int64_t, OrderType> HumanOrderTypes;
  std::map<std::int64_t, Side> HumanOrderSides;
  std::vector<ActionMap> AlgoActions;
  std::vector<Pending> PendingList;
  std::map<std::tuple<std::size_t, int, std::int64_t>, HumanOutcome> Outcome;
  std::set<std::tuple<std::size_t, int, std::int64_t>> MatchedAlgoActions;
};

} // namespace

DivergenceResult analyzeDivergences(
    const SessionSpec &Spec, std::shared_ptr<const Timeline> TimelineRef,
    std::span<const JournalEntry> Human, std::int64_t LastStep,
    const std::vector<AlgorithmInput> &Algorithms,
    const std::optional<BenchmarkSeries> &Benchmark,
    const std::optional<RuoniaSeries> &Ruonia,
    std::int32_t RuoniaPreviousDayYmd, std::function<bool()> ShouldCancel) {
  Analyzer Runner(Spec, std::move(TimelineRef), Human, LastStep, Algorithms,
                  Benchmark, Ruonia, RuoniaPreviousDayYmd,
                  std::move(ShouldCancel));
  return Runner.run();
}

} // namespace core
