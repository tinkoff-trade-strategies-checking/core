// Автотесты на инварианты INV-3 и INV-9 после подключения DSL (core#62).
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "core/engine/digest.hpp"
#include "core/engine/journal.hpp"
#include "core/engine/session_engine.hpp"
#include "core/exec/costs.hpp"
#include "core/exec/order.hpp"
#include "core/market/bar.hpp"
#include "core/market/calendar_source.hpp"
#include "core/market/date_utils.hpp"
#include "core/market/in_memory_bar_source.hpp"
#include "core/market/instrument.hpp"
#include "core/market/timeframe.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp"
#include "core/report/benchmark.hpp"
#include "core/report/metrics.hpp"
#include "core/report/ruonia.hpp"
#include "core/strategy/strategy_runner.hpp"
#include "dsl/check.hpp"
#include "dsl/interpreter.hpp"
#include "dsl/types.hpp"
#include "support/fixture1.hpp"

namespace core {
namespace {

struct SimpleBarSpec {
  std::string Open;
  std::string High;
  std::string Low;
  std::string Close;
  std::int64_t Volume = 10000;
};

std::shared_ptr<const Timeline>
makeSingleTimeline(const std::vector<SimpleBarSpec> &BarsSpec,
                   const std::string &StepStr = "0.01",
                   std::int64_t LotSize = 1,
                   const std::vector<SimpleBarSpec> &WarmupSpec = {},
                   std::int32_t StartYmd = 20240201) {
  std::vector<Instrument> InstrList{
      Instrument{"A", "TEST", LotSize, Decimal::parse(StepStr)}};
  std::vector<Bar> MainBars;
  std::vector<Bar> WarmupBars;
  std::vector<TradingDay> Calendar;
  const std::int64_t BaseMs = moscowDayStartMs(StartYmd);

  std::int64_t DayIdx = 0;
  auto PushBar = [&](const SimpleBarSpec &SpecItem, std::vector<Bar> &Out) {
    const std::int64_t OpenMs = BaseMs + DayIdx * kDayMs;
    ++DayIdx;
    Bar Item;
    Item.TsOpenMs = OpenMs;
    Item.AvailableAtMs = OpenMs + kDayMs;
    Item.Open = Decimal::parse(SpecItem.Open);
    Item.High = Decimal::parse(SpecItem.High);
    Item.Low = Decimal::parse(SpecItem.Low);
    Item.Close = Decimal::parse(SpecItem.Close);
    Item.Volume = SpecItem.Volume;
    Item.Value = Item.Close.times(Item.Volume);
    Out.push_back(Item);
    Calendar.push_back(TradingDay{moscowYmd(OpenMs), true});
  };

  for (const SimpleBarSpec &Item : WarmupSpec) {
    PushBar(Item, WarmupBars);
  }
  for (const SimpleBarSpec &Item : BarsSpec) {
    PushBar(Item, MainBars);
  }

  Timeline MainTl = Timeline::build(InstrList, {MainBars}, Calendar);
  if (!WarmupBars.empty()) {
    const Timeline HistTl = Timeline::build(InstrList, {WarmupBars}, Calendar);
    const auto WarmupCount = static_cast<std::int64_t>(WarmupBars.size());
    MainTl = MainTl.withWarmup(HistTl, WarmupCount);
  }
  return std::make_shared<const Timeline>(std::move(MainTl));
}

SessionSpec makeSessionSpec(const std::shared_ptr<const Timeline> &Tl,
                            const std::string &CashStr = "1000000",
                            CostConfig Costs = CostConfig::zero()) {
  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs = std::move(Costs);
  Spec.InitialCash = Decimal::parse(CashStr);
  return Spec;
}

std::vector<JournalEntry> stripRuleMeta(std::vector<JournalEntry> Journal) {
  for (JournalEntry &Entry : Journal) {
    Entry.RuleName.clear();
    Entry.Because.clear();
  }
  return Journal;
}

bool sameOrderExecution(const Order &Left, const Order &Right) {
  return Left.Id == Right.Id && Left.Spec == Right.Spec &&
         Left.PlacedStep == Right.PlacedStep && Left.Status == Right.Status &&
         Left.ReasonCode == Right.ReasonCode &&
         Left.InvalidField == Right.InvalidField &&
         Left.FilledLots == Right.FilledLots &&
         Left.OcoSiblingId == Right.OcoSiblingId &&
         Left.TrailingPeak == Right.TrailingPeak &&
         Left.TrailingStopLevel == Right.TrailingStopLevel &&
         Left.Triggered == Right.Triggered &&
         Left.ClosedStep == Right.ClosedStep;
}

bool sameFill(const Fill &Left, const Fill &Right) {
  return Left.OrderId == Right.OrderId && Left.Step == Right.Step &&
         Left.Instrument == Right.Instrument &&
         Left.OrderSide == Right.OrderSide && Left.Lots == Right.Lots &&
         Left.Price == Right.Price && Left.BasePrice == Right.BasePrice &&
         Left.Value == Right.Value && Left.Commission == Right.Commission &&
         Left.Fee == Right.Fee && Left.SpreadCost == Right.SpreadCost &&
         Left.SlippageCost == Right.SlippageCost &&
         Left.StopAndTakeSameBar == Right.StopAndTakeSameBar &&
         Left.Seq == Right.Seq && Left.cost() == Right.cost();
}

bool sameRoundTrip(const RoundTrip &Left, const RoundTrip &Right) {
  return Left.Instrument == Right.Instrument &&
         Left.EntryStep == Right.EntryStep && Left.ExitStep == Right.ExitStep &&
         Left.EntryOrderId == Right.EntryOrderId && Left.Lots == Right.Lots &&
         Left.EntryPrice == Right.EntryPrice &&
         Left.ExitPrice == Right.ExitPrice &&
         Left.EntryValue == Right.EntryValue &&
         Left.ExitValue == Right.ExitValue && Left.Costs == Right.Costs &&
         Left.NetPnl == Right.NetPnl;
}
bool sameCommandResult(const CommandResult &Left, const CommandResult &Right) {
  return Left.Seq == Right.Seq && Left.Accepted == Right.Accepted &&
         Left.ReasonCode == Right.ReasonCode && Left.Field == Right.Field &&
         Left.OrderIds == Right.OrderIds;
}

std::optional<std::string>
findExecutionMismatch(const StrategyRunResult &AlgoRes,
                      const ReplayResult &HumanRes) {
  if (!AlgoRes.Compiled || AlgoRes.Invalid || !AlgoRes.Engine.has_value()) {
    return "AlgoRes не скомпилирован или не содержит SessionEngine";
  }
  const SessionEngine &Algo = *AlgoRes.Engine;
  const SessionEngine &Human = HumanRes.Engine;

  if (digestOf(Algo) != digestOf(Human)) {
    return "digestOf(Algo) != digestOf(Human)";
  }
  if (Algo.status() != Human.status() || HumanRes.Status != Algo.status()) {
    return "Не совпал SessionStatus";
  }
  if (Algo.currentStep() != Human.currentStep() ||
      HumanRes.LastStep != Algo.currentStep()) {
    return "Не совпал currentStep / LastStep";
  }
  if (Algo.fills().size() != Human.fills().size()) {
    return "Не совпал размер fills()";
  }
  for (std::size_t I = 0; I < Algo.fills().size(); ++I) {
    if (!sameFill(Algo.fills()[I], Human.fills()[I])) {
      return "Расхождение в Fill #" + std::to_string(I);
    }
  }
  if (Algo.orders().size() != Human.orders().size()) {
    return "Не совпал размер orders()";
  }
  for (std::size_t I = 0; I < Algo.orders().size(); ++I) {
    if (!sameOrderExecution(Algo.orders()[I], Human.orders()[I])) {
      return "Расхождение в Order #" + std::to_string(I);
    }
  }
  if (Algo.portfolio().cash() != Human.portfolio().cash()) {
    return "Не совпал portfolio().cash()";
  }
  const auto AlgoSnap = Algo.snapshot();
  const auto HumanSnap = Human.snapshot();
  if (AlgoSnap.Lots.size() != HumanSnap.Lots.size()) {
    return "Не совпало число бумаг в snapshot()";
  }
  for (std::size_t Inst = 0; Inst < AlgoSnap.Lots.size(); ++Inst) {
    const int Idx = static_cast<int>(Inst);
    if (Algo.portfolio().lots(Idx) != Human.portfolio().lots(Idx) ||
        Algo.portfolio().avgPrice(Idx) != Human.portfolio().avgPrice(Idx) ||
        Algo.portfolio().costBasis(Idx) != Human.portfolio().costBasis(Idx)) {
      return "Расхождение в Portfolio по бумаге #" + std::to_string(Inst);
    }
  }
  if (Algo.equity().size() != Human.equity().size()) {
    return "Не совпал размер equity()";
  }
  for (std::size_t S = 0; S < Algo.equity().size(); ++S) {
    if (Algo.equity()[S].Step != Human.equity()[S].Step ||
        Algo.equity()[S].Equity != Human.equity()[S].Equity) {
      return "Расхождение в equity() на шаге #" + std::to_string(S);
    }
  }
  if (Algo.positionValues() != Human.positionValues()) {
    return "Расхождение в positionValues()";
  }
  const auto &AlgoRt = Algo.portfolio().roundTrips();
  const auto &HumanRt = Human.portfolio().roundTrips();
  if (AlgoRt.size() != HumanRt.size()) {
    return "Не совпал размер roundTrips()";
  }
  for (std::size_t I = 0; I < AlgoRt.size(); ++I) {
    if (!sameRoundTrip(AlgoRt[I], HumanRt[I])) {
      return "Расхождение в RoundTrip #" + std::to_string(I);
    }
  }
  if (AlgoRes.CommandResults.size() != HumanRes.CommandResults.size()) {
    return "Не совпал размер CommandResults";
  }
  for (std::size_t I = 0; I < AlgoRes.CommandResults.size(); ++I) {
    if (!sameCommandResult(AlgoRes.CommandResults[I],
                           HumanRes.CommandResults[I])) {
      return "Расхождение в CommandResult #" + std::to_string(I);
    }
  }
  return std::nullopt;
}

void assertSameExecution(const StrategyRunResult &AlgoRes,
                         const ReplayResult &HumanRes) {
  const std::optional<std::string> Mismatch =
      findExecutionMismatch(AlgoRes, HumanRes);
  EXPECT_FALSE(Mismatch.has_value()) << (Mismatch ? *Mismatch : "");
}

void rcAssertSameExecution(const StrategyRunResult &AlgoRes,
                           const ReplayResult &HumanRes) {
  const std::optional<std::string> Mismatch =
      findExecutionMismatch(AlgoRes, HumanRes);
  RC_ASSERT(!Mismatch.has_value());
}

CostConfig genRandomCostConfig() {
  CostConfig Costs;
  const auto CommBps = *rc::gen::inRange<std::int64_t>(0, 50);
  const auto FeeBps = *rc::gen::inRange<std::int64_t>(0, 15);
  const auto SpreadBps = *rc::gen::inRange<std::int64_t>(0, 40);
  const auto KappaPct = *rc::gen::inRange<std::int64_t>(0, 30);
  Costs.CommissionRate = Decimal::parse("0.0001").times(CommBps);
  Costs.ExchangeFeeRate = Decimal::parse("0.0001").times(FeeBps);
  Costs.Spread = Decimal::parse("0.0001").times(SpreadBps);
  Costs.SlippageKappa = Decimal::parse("0.01").times(KappaPct);
  if (*rc::gen::arbitrary<bool>()) {
    const auto SharePct = *rc::gen::inRange<std::int64_t>(5, 101);
    Costs.LiquidityShare = Decimal::parse("0.01").times(SharePct);
  }
  return Costs;
}

std::shared_ptr<const Timeline> genRandomTimeline(std::int64_t &WarmupOut) {
  const std::size_t NumInstr = *rc::gen::inRange<std::size_t>(1, 4);
  const std::int64_t WarmupDays = *rc::gen::inRange<std::int64_t>(0, 4);
  const std::int64_t MainDays = *rc::gen::inRange<std::int64_t>(2, 6);
  const std::int64_t SlotsPerDay = *rc::gen::inRange<std::int64_t>(1, 4);

  std::vector<Instrument> InstrList;
  for (std::size_t I = 0; I < NumInstr; ++I) {
    const std::string Key(1, static_cast<char>('A' + I));
    const std::string Ticker = "SYM" + std::to_string(I);
    const std::int64_t LotSize = *rc::gen::element<std::int64_t>(1, 10, 100);
    const std::string StepText =
        *rc::gen::element<std::string>("0.01", "0.05", "0.1", "0.5");
    InstrList.push_back(
        Instrument{Key, Ticker, LotSize, Decimal::parse(StepText)});
  }

  const std::int64_t BaseDayMs = moscowDayStartMs(20240110);
  std::vector<TradingDay> Calendar;
  std::vector<std::vector<Bar>> WarmupBars(NumInstr);
  std::vector<std::vector<Bar>> MainBars(NumInstr);
  std::vector<std::int64_t> LastCloseCents(NumInstr);
  for (std::size_t I = 0; I < NumInstr; ++I) {
    LastCloseCents[I] = *rc::gen::inRange<std::int64_t>(5000, 15000);
  }
  auto MakeBar = [&](std::size_t Inst, std::int64_t OpenMs) {
    const std::int64_t Delta = *rc::gen::inRange<std::int64_t>(-400, 401);
    const std::int64_t OpenCents = LastCloseCents[Inst];
    const std::int64_t CloseCents =
        std::max<std::int64_t>(2000, OpenCents + Delta);
    const std::int64_t HighCents = std::max(OpenCents, CloseCents) +
                                   *rc::gen::inRange<std::int64_t>(50, 300);
    const std::int64_t LowCents = std::max<std::int64_t>(
        1000, std::min(OpenCents, CloseCents) -
                  *rc::gen::inRange<std::int64_t>(50, 300));
    LastCloseCents[Inst] = CloseCents;

    const Decimal Cent = Decimal::parse("0.01");
    Bar Item;
    Item.TsOpenMs = OpenMs;
    Item.AvailableAtMs = OpenMs + kHourMs;
    Item.Open = Cent.times(OpenCents);
    Item.High = Cent.times(HighCents);
    Item.Low = Cent.times(LowCents);
    Item.Close = Cent.times(CloseCents);
    const std::int64_t VolLots = *rc::gen::inRange<std::int64_t>(20, 400);
    Item.Volume = VolLots * InstrList[Inst].LotSize;
    Item.Value = Item.Close.times(Item.Volume);
    return Item;
  };

  for (std::int64_t DayIdx = 0; DayIdx < WarmupDays + MainDays; ++DayIdx) {
    const std::int64_t DayStartMs = BaseDayMs + DayIdx * kDayMs;
    Calendar.push_back(TradingDay{moscowYmd(DayStartMs), true});
    const bool IsWarmup = DayIdx < WarmupDays;
    for (std::int64_t Slot = 0; Slot < SlotsPerDay; ++Slot) {
      const std::int64_t OpenMs = DayStartMs + (10 + Slot) * kHourMs;
      for (std::size_t Inst = 0; Inst < NumInstr; ++Inst) {
        const bool MustKeep =
            IsWarmup || Inst == 0 || (DayIdx == WarmupDays && Slot == 0);
        if (!MustKeep && *rc::gen::inRange(0, 5) == 0) {
          continue;
        }
        auto &Target = IsWarmup ? WarmupBars[Inst] : MainBars[Inst];
        Target.push_back(MakeBar(Inst, OpenMs));
      }
    }
  }

  WarmupOut = WarmupDays * SlotsPerDay;
  Timeline MainTl = Timeline::build(InstrList, MainBars, Calendar);
  if (WarmupDays > 0) {
    const Timeline HistTl = Timeline::build(InstrList, WarmupBars, Calendar);
    MainTl = MainTl.withWarmup(HistTl, WarmupOut);
  }
  return std::make_shared<const Timeline>(std::move(MainTl));
}

std::string genRandomDslSource(std::int64_t WarmupSteps) {
  std::string Src = "param risk_pct = 95\n";
  int RuleIdx = 0;
  auto AddRule = [&](const std::string &When, const std::string &Then,
                     const std::string &Because) {
    ++RuleIdx;
    Src += "rule \"r" + std::to_string(RuleIdx) + "\"\n";
    Src += "  when " + When + "\n";
    Src += "  then " + Then + "\n";
    Src += "  because \"" + Because + "\"\n";
  };

  const int EntryCount = *rc::gen::inRange(1, 3);
  for (int I = 0; I < EntryCount; ++I) {
    std::string Cond = "position == 0";
    if (WarmupSteps >= 3 && *rc::gen::arbitrary<bool>()) {
      Cond += " and sma(close, 2) >= sma(close, 3)";
    } else if (*rc::gen::arbitrary<bool>()) {
      Cond += " and close >= open";
    }
    const std::string Size = *rc::gen::element<std::string>(
        "1 lots", "2 lots", "40 % cash", "30 % equity");
    const std::string Ord = *rc::gen::element<std::string>(
        "market", "limit close * 0.98", "stop high * 1.01");
    const std::string Tif = *rc::gen::element<std::string>("", " day", " gtc");
    AddRule(Cond, "buy " + Size + " " + Ord + Tif, "вход {close}");
  }

  const int ExitCount = *rc::gen::inRange(1, 3);
  for (int I = 0; I < ExitCount; ++I) {
    std::string Cond = "position > 0";
    if (*rc::gen::arbitrary<bool>()) {
      Cond += " and bars_in_position >= 1";
    }
    const std::string Act = *rc::gen::element<std::string>(
        "sell all", "sell 50 % position market",
        "sell all limit entry_price * 1.03", "sell all stop entry_price * 0.95",
        "sell all take_profit entry_price * 1.05");
    const std::string Tif = *rc::gen::element<std::string>("", " day", " gtc");
    AddRule(Cond, Act + Tif, "выход {entry_price}");
  }

  if (*rc::gen::arbitrary<bool>()) {
    AddRule("position == 0 and close < open", "cancel", "отмена при снижении");
  }
  return Src;
}

RC_GTEST_PROP(StrategyInvariantsProperty, HumanReplayMatchesStrategyRun, ()) {
  std::int64_t WarmupSteps = 0;
  const std::shared_ptr<const Timeline> Tl = genRandomTimeline(WarmupSteps);
  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs = genRandomCostConfig();
  Spec.InitialCash =
      Decimal::fromUnits(*rc::gen::inRange<std::int64_t>(5000, 200000));

  StrategyRunOptions Opts;
  Opts.Source = genRandomDslSource(WarmupSteps);
  const StrategyRunResult AlgoRes = runStrategy(Spec, Tl, Opts);
  RC_ASSERT(AlgoRes.Compiled);
  RC_ASSERT(!AlgoRes.Invalid);
  RC_ASSERT(AlgoRes.Engine.has_value());

  const std::vector<JournalEntry> HumanJournal = stripRuleMeta(AlgoRes.Journal);
  const ReplayResult HumanRes =
      replaySession(Spec, Tl, HumanJournal, Tl->totalSteps() - 1, 0);
  rcAssertSameExecution(AlgoRes, HumanRes);
}
TEST(StrategyInvariants, SameCostsSpreadSlippageAndLiquidityLimit) {
  const auto Tl = makeSingleTimeline(
      {
          SimpleBarSpec{"100", "102", "99", "100", 500},
          SimpleBarSpec{"100", "102", "99", "101", 500},
          SimpleBarSpec{"101", "102", "100", "101", 500},
          SimpleBarSpec{"101", "103", "100", "103", 500},
          SimpleBarSpec{"103", "105", "102", "104", 2000},
          SimpleBarSpec{"104", "105", "103", "104", 2000},
      },
      "0.01", 10);

  const std::string Source = R"dsl(
rule "вход"
  when position == 0 and close == 100
  then buy 8 lots market
  because "вход по рынку"

rule "добор"
  when position == 5 and close == 101
  then buy 7 lots limit 102
  because "добор лимитной"

rule "выход"
  when position >= 12 and close >= 103
  then sell all market
  because "выход по рынку"
)dsl";

  StrategyRunOptions Opts;
  Opts.Source = Source;
  const SessionSpec Spec =
      makeSessionSpec(Tl, "100000", CostConfig::defaults());
  const StrategyRunResult AlgoRes = runStrategy(Spec, Tl, Opts);
  ASSERT_TRUE(AlgoRes.Compiled);
  ASSERT_TRUE(AlgoRes.Engine.has_value());

  bool FoundLiquidityCancel = false;
  for (const Order &Ord : AlgoRes.Engine->orders()) {
    if (Ord.Status == OrderStatus::Cancelled &&
        Ord.ReasonCode == Reason::LiquidityLimit) {
      FoundLiquidityCancel = true;
    }
  }
  EXPECT_TRUE(FoundLiquidityCancel);

  bool FoundAllCostParts = false;
  for (const Fill &FillItem : AlgoRes.Engine->fills()) {
    if (FillItem.Commission.sign() > 0 && FillItem.Fee.sign() > 0 &&
        FillItem.SpreadCost.sign() > 0 && FillItem.SlippageCost.sign() > 0) {
      FoundAllCostParts = true;
    }
  }
  EXPECT_TRUE(FoundAllCostParts);
  EXPECT_FALSE(AlgoRes.Engine->portfolio().roundTrips().empty());

  const std::vector<JournalEntry> HumanJournal = stripRuleMeta(AlgoRes.Journal);
  const ReplayResult HumanRes =
      replaySession(Spec, Tl, HumanJournal, Tl->totalSteps() - 1, 0);
  assertSameExecution(AlgoRes, HumanRes);

  const StrategyRunResult ZeroCostRes =
      runStrategy(makeSessionSpec(Tl, "100000", CostConfig::zero()), Tl, Opts);
  ASSERT_TRUE(ZeroCostRes.Engine.has_value());
  EXPECT_NE(digestOf(*AlgoRes.Engine), digestOf(*ZeroCostRes.Engine));
}

TEST(StrategyInvariants, SameReplaceAndDayExpirationAndPositionClosed) {
  const auto Tl = makeSingleTimeline(
      {
          SimpleBarSpec{"91", "92", "90", "91", 100000},
          SimpleBarSpec{"120", "121", "90", "120", 100000},
          SimpleBarSpec{"92", "93", "90", "92", 100000},
          SimpleBarSpec{"95", "96", "88", "100", 100000},
          SimpleBarSpec{"100", "103", "99", "102", 100000},
          SimpleBarSpec{"102", "104", "101", "103", 100000},
          SimpleBarSpec{"103", "105", "102", "104", 100000},
      },
      "0.01", 1);

  const std::string Source = R"dsl(
rule "лимит_замена"
  when position == 0 and close != 100
  then buy (close - 90) lots limit 89
  because "лимит при close={close}"

rule "дневная"
  when position == 0 and close == 91
  then buy 1 lots limit 40 day
  because "дневная заявка"

rule "стоп"
  when position > 0
  then sell all stop 95
  because "защитный стоп"

rule "тейк"
  when position > 0
  then sell all take_profit 110
  because "тейк-профит"

rule "выход"
  when position > 0 and close >= 102
  then sell all market
  because "закрытие позиции"
)dsl";

  StrategyRunOptions Opts;
  Opts.Source = Source;
  const SessionSpec Spec = makeSessionSpec(Tl, "1000", CostConfig::defaults());
  const StrategyRunResult AlgoRes = runStrategy(Spec, Tl, Opts);
  ASSERT_TRUE(AlgoRes.Compiled);
  ASSERT_TRUE(AlgoRes.Engine.has_value());

  bool HasRejectedReplace = false;
  bool HasAcceptedReplace = false;
  for (std::size_t I = 0; I < AlgoRes.Journal.size(); ++I) {
    if (std::holds_alternative<ReplaceCmd>(AlgoRes.Journal[I].Command)) {
      if (!AlgoRes.CommandResults[I].Accepted &&
          AlgoRes.CommandResults[I].ReasonCode == Reason::InsufficientCash) {
        HasRejectedReplace = true;
      }
      if (AlgoRes.CommandResults[I].Accepted) {
        HasAcceptedReplace = true;
      }
    }
  }
  EXPECT_TRUE(HasRejectedReplace);
  EXPECT_TRUE(HasAcceptedReplace);

  bool HasReplacedOrder = false;
  bool HasDayExpired = false;
  int PositionClosedCount = 0;
  for (const Order &Ord : AlgoRes.Engine->orders()) {
    if (Ord.Status == OrderStatus::Cancelled &&
        Ord.ReasonCode == Reason::Replaced) {
      HasReplacedOrder = true;
    }
    if (Ord.Status == OrderStatus::Expired &&
        Ord.ReasonCode == Reason::DayExpired && Ord.ClosedStep == 1) {
      HasDayExpired = true;
    }
    if (Ord.Status == OrderStatus::Cancelled &&
        Ord.ReasonCode == Reason::PositionClosed && Ord.ClosedStep == 5) {
      ++PositionClosedCount;
    }
  }
  EXPECT_TRUE(HasReplacedOrder);
  EXPECT_TRUE(HasDayExpired);
  EXPECT_EQ(PositionClosedCount, 2);

  const std::vector<JournalEntry> HumanJournal = stripRuleMeta(AlgoRes.Journal);
  const ReplayResult HumanRes =
      replaySession(Spec, Tl, HumanJournal, Tl->totalSteps() - 1, 0);
  assertSameExecution(AlgoRes, HumanRes);

  std::vector<JournalEntry> NoReplaceJournal = HumanJournal;
  for (std::size_t I = 0; I < NoReplaceJournal.size(); ++I) {
    if (std::holds_alternative<ReplaceCmd>(NoReplaceJournal[I].Command) &&
        AlgoRes.CommandResults[I].Accepted) {
      NoReplaceJournal[I].Command = CancelCmd{999, Reason::CancelledByUser};
    }
  }
  const ReplayResult NoReplaceRes =
      replaySession(Spec, Tl, NoReplaceJournal, Tl->totalSteps() - 1, 0);
  EXPECT_NE(digestOf(*AlgoRes.Engine), digestOf(NoReplaceRes.Engine));
}
TEST(StrategyInvariants, MultiInstrumentSharedCashAndSkippedBar) {
  auto Fix = testing::fixture1();
  const auto Tl = std::make_shared<const Timeline>(
      Timeline::load(Fix.Bars, Fix.Calendar, Fix.SnapshotId, Fix.Instruments,
                     Timeframe::D1, 20240109, 20240122));
  const std::string Source = R"dsl(
rule "вход"
  when position == 0
  then buy 95 % cash
  because "покупка на 95% кэша"

rule "выход"
  when position > 0 and bars_in_position >= 2
  then sell all
  because "закрытие на баре {bars_in_position}"
)dsl";

  StrategyRunOptions Opts;
  Opts.Source = Source;
  const SessionSpec Spec =
      makeSessionSpec(Tl, "100000", CostConfig::defaults());
  const StrategyRunResult AlgoRes = runStrategy(Spec, Tl, Opts);
  ASSERT_TRUE(AlgoRes.Compiled);
  ASSERT_TRUE(AlgoRes.Engine.has_value());

  bool HasFillA = false;
  bool HasFillB = false;
  for (const Fill &FillItem : AlgoRes.Engine->fills()) {
    if (FillItem.Instrument == 0) {
      HasFillA = true;
    } else if (FillItem.Instrument == 1) {
      HasFillB = true;
    }
  }
  EXPECT_TRUE(HasFillA);
  EXPECT_TRUE(HasFillB);

  bool FoundSellAOnStep3 = false;
  bool FoundSellBOnStep4 = false;
  for (const Order &Ord : AlgoRes.Engine->orders()) {
    if (Ord.Spec.OrderSide == Side::Sell && Ord.Spec.Instrument == 0 &&
        Ord.PlacedStep == 3) {
      FoundSellAOnStep3 = true;
    }
    if (Ord.Spec.OrderSide == Side::Sell && Ord.Spec.Instrument == 1 &&
        Ord.PlacedStep == 4) {
      FoundSellBOnStep4 = true;
    }
  }
  EXPECT_TRUE(FoundSellAOnStep3);
  EXPECT_TRUE(FoundSellBOnStep4);

  const std::vector<JournalEntry> HumanJournal = stripRuleMeta(AlgoRes.Journal);
  const ReplayResult HumanRes =
      replaySession(Spec, Tl, HumanJournal, Tl->totalSteps() - 1, 0);
  assertSameExecution(AlgoRes, HumanRes);

  const StrategyRunResult ZeroCostRes =
      runStrategy(makeSessionSpec(Tl, "100000", CostConfig::zero()), Tl, Opts);
  ASSERT_TRUE(ZeroCostRes.Engine.has_value());
  EXPECT_NE(digestOf(*AlgoRes.Engine), digestOf(*ZeroCostRes.Engine));
}

template <typename T>
concept HasNoExternalChannel =
    !requires(T Obj) { Obj.News; } && !requires(T Obj) { Obj.news; } &&
    !requires(T Obj) { Obj.Headline; } && !requires(T Obj) { Obj.headline; } &&
    !requires(T Obj) { Obj.Date; } && !requires(T Obj) { Obj.date; } &&
    !requires(T Obj) { Obj.Time; } && !requires(T Obj) { Obj.time; } &&
    !requires(T Obj) { Obj.Timestamp; } &&
    !requires(T Obj) { Obj.timestamp; } && !requires(T Obj) { Obj.TsOpenMs; } &&
    !requires(T Obj) { Obj.AvailableAtMs; } &&
    !requires(T Obj) { Obj.Ticker; } && !requires(T Obj) { Obj.ticker; } &&
    !requires(T Obj) { Obj.SecId; } && !requires(T Obj) { Obj.secid; } &&
    !requires(T Obj) { Obj.Sector; } && !requires(T Obj) { Obj.sector; } &&
    !requires(T Obj) { Obj.Calendar; } && !requires(T Obj) { Obj.calendar; } &&
    !requires(T Obj) { Obj.Benchmark; } &&
    !requires(T Obj) { Obj.benchmark; } && !requires(T Obj) { Obj.Ruonia; } &&
    !requires(T Obj) { Obj.ruonia; } && !requires(T Obj) { Obj.Http; } &&
    !requires(T Obj) { Obj.http; } && !requires(T Obj) { Obj.Client; } &&
    !requires(T Obj) { Obj.client; } && !requires(T Obj) { Obj.Callback; } &&
    !requires(T Obj) { Obj.callback; };

TEST(StrategyInvariants, DslAndRunnerTypesHaveNoNewsOrExternalChannels) {
  static_assert(std::is_same_v<decltype(dsl::Bar::Step), std::int64_t>);
  static_assert(std::is_same_v<decltype(dsl::Bar::Open), double>);
  static_assert(std::is_same_v<decltype(dsl::Bar::High), double>);
  static_assert(std::is_same_v<decltype(dsl::Bar::Low), double>);
  static_assert(std::is_same_v<decltype(dsl::Bar::Close), double>);
  static_assert(std::is_same_v<decltype(dsl::Bar::Volume), double>);
  static_assert(sizeof(dsl::Bar) == sizeof(std::int64_t) + 5 * sizeof(double));

  static_assert(
      std::is_same_v<decltype(dsl::PortfolioSnapshot::Position), std::int64_t>);
  static_assert(
      std::is_same_v<decltype(dsl::PortfolioSnapshot::EntryPrice), double>);
  static_assert(std::is_same_v<decltype(dsl::PortfolioSnapshot::Cash), double>);
  static_assert(
      std::is_same_v<decltype(dsl::PortfolioSnapshot::Equity), double>);
  static_assert(std::is_same_v<decltype(dsl::PortfolioSnapshot::BarsInPosition),
                               std::optional<std::int64_t>>);
  static_assert(
      std::is_same_v<decltype(dsl::PortfolioSnapshot::LotSize), std::int64_t>);
  static_assert(
      std::is_same_v<decltype(dsl::PortfolioSnapshot::PositionReopenedOnBar),
                     bool>);
  struct ExpectedPortfolioSnapshot {
    std::int64_t Position = 0;
    double EntryPrice = 0.0;
    double Cash = 0.0;
    double Equity = 0.0;
    std::optional<std::int64_t> BarsInPosition;
    std::int64_t LotSize = 1;
    bool PositionReopenedOnBar = false;
  };
  static_assert(sizeof(dsl::PortfolioSnapshot) ==
                sizeof(ExpectedPortfolioSnapshot));

  static_assert(HasNoExternalChannel<dsl::Bar>);
  static_assert(HasNoExternalChannel<dsl::PortfolioSnapshot>);
  static_assert(HasNoExternalChannel<dsl::RuleIntent>);
  static_assert(HasNoExternalChannel<dsl::CheckResult>);
  static_assert(HasNoExternalChannel<dsl::CompileResult>);
  static_assert(HasNoExternalChannel<dsl::StepOutcome>);
  static_assert(HasNoExternalChannel<StrategyRunOptions>);
  static_assert(HasNoExternalChannel<StrategyRunResult>);

  using RunStrategyFn = StrategyRunResult (*)(const SessionSpec &,
                                              std::shared_ptr<const Timeline>,
                                              const StrategyRunOptions &);
  using InstanceStepFn = dsl::StepOutcome (dsl::StrategyInstance::*)(
      std::size_t, const dsl::Bar &, const dsl::PortfolioSnapshot &, bool);
  using ParamMap = std::map<std::string, double>;
  using DslCheckFn = dsl::CheckResult (*)(std::string_view, const ParamMap &);
  using DslCompileFn =
      dsl::CompileResult (*)(std::string_view, const ParamMap &, std::size_t);

  static_assert(std::is_same_v<decltype(&runStrategy), RunStrategyFn>);
  static_assert(
      std::is_same_v<decltype(&dsl::StrategyInstance::step), InstanceStepFn>);
  static_assert(std::is_same_v<decltype(&dsl::check), DslCheckFn>);
  static_assert(std::is_same_v<decltype(&dsl::compile), DslCompileFn>);

  EXPECT_TRUE((HasNoExternalChannel<dsl::Bar>));
  EXPECT_TRUE((HasNoExternalChannel<StrategyRunOptions>));
  EXPECT_TRUE((HasNoExternalChannel<StrategyRunResult>));
}
bool hasDiagnosticCode(const std::vector<dsl::Diagnostic> &Diagnostics,
                       std::string_view Code) {
  for (const dsl::Diagnostic &Diag : Diagnostics) {
    if (Diag.Code == Code) {
      return true;
    }
  }
  return false;
}

void expectRejectedWithEType(const std::string &Source,
                             const std::map<std::string, double> &Overrides,
                             const std::shared_ptr<const Timeline> &Tl) {
  const dsl::CheckResult CheckRes = dsl::check(Source, Overrides);
  EXPECT_TRUE(hasDiagnosticCode(CheckRes.Diagnostics, "E-TYPE")) << Source;

  const dsl::CompileResult CompileRes = dsl::compile(Source, Overrides, 1);
  EXPECT_FALSE(CompileRes.Ok) << Source;
  EXPECT_EQ(CompileRes.Instance, nullptr) << Source;
  EXPECT_TRUE(hasDiagnosticCode(CompileRes.Diagnostics, "E-TYPE")) << Source;

  StrategyRunOptions Opts;
  Opts.Source = Source;
  Opts.ParamOverrides = Overrides;
  const StrategyRunResult RunRes = runStrategy(makeSessionSpec(Tl), Tl, Opts);
  EXPECT_FALSE(RunRes.Compiled) << Source;
  EXPECT_FALSE(RunRes.Engine.has_value()) << Source;
  EXPECT_TRUE(hasDiagnosticCode(RunRes.Diagnostics, "E-TYPE")) << Source;
}

TEST(StrategyInvariants, DslCheckCompileStepRejectsNewsAndExternalIdentifiers) {
  const auto Tl = makeSingleTimeline({
      SimpleBarSpec{"100", "101", "99", "100"},
      SimpleBarSpec{"100", "101", "99", "100"},
  });

  std::istringstream ForbiddenStream(
      "news headline sentiment feed date time timestamp ymd secid ticker "
      "sector imoex benchmark ruonia calendar http fetch env file rand random "
      "now today");

  const std::string LimitHead = "rule \"r\" when true then buy 1 lots limit ";
  std::string IdStr;
  while (ForbiddenStream >> IdStr) {
    const std::string WhenSrc =
        "rule \"r\" when " + IdStr + " > 0 then cancel because \"x\"";
    const std::string SizeSrc =
        "rule \"r\" when true then buy " + IdStr + " lots market because \"x\"";
    const std::string PriceSrc = LimitHead + IdStr + " because \"x\"";
    const std::string BecauseSrc =
        "rule \"r\" when true then buy 1 lots market because \"x={" + IdStr +
        "}\"";
    const std::string ParamSrc =
        "param p = 1\nrule \"r\" when close > p then buy 1 lots market "
        "because \"x\"";
    expectRejectedWithEType(WhenSrc, {}, Tl);
    expectRejectedWithEType(SizeSrc, {}, Tl);
    expectRejectedWithEType(PriceSrc, {}, Tl);
    expectRejectedWithEType(BecauseSrc, {}, Tl);
    expectRejectedWithEType(ParamSrc, {{IdStr, 1.0}}, Tl);
  }

  const std::string ParamDefaultIdent =
      "param p = news\nrule \"r\" when true then cancel because \"тест\"";
  const dsl::CheckResult ParamCheck = dsl::check(ParamDefaultIdent);
  EXPECT_TRUE(hasDiagnosticCode(ParamCheck.Diagnostics, "E-SYN"));
  EXPECT_FALSE(dsl::compile(ParamDefaultIdent, {}, 1).Ok);
}

bool sameJournalEntry(const JournalEntry &Left, const JournalEntry &Right) {
  if (Left.Seq != Right.Seq || Left.Step != Right.Step ||
      Left.RuleName != Right.RuleName || Left.Because != Right.Because ||
      Left.Command.index() != Right.Command.index()) {
    return false;
  }
  if (const auto *Lp = std::get_if<PlaceCmd>(&Left.Command)) {
    const auto &Rp = std::get<PlaceCmd>(Right.Command);
    return Lp->Order == Rp.Order && Lp->OcoWith == Rp.OcoWith;
  }
  if (const auto *Lc = std::get_if<CancelCmd>(&Left.Command)) {
    const auto &Rc = std::get<CancelCmd>(Right.Command);
    return Lc->OrderId == Rc.OrderId && Lc->ReasonCode == Rc.ReasonCode;
  }
  const auto &Lr = std::get<ReplaceCmd>(Left.Command);
  const auto &Rr = std::get<ReplaceCmd>(Right.Command);
  return Lr.OrderId == Rr.OrderId &&
         Lr.Replacement.Order == Rr.Replacement.Order &&
         Lr.Replacement.OcoWith == Rr.Replacement.OcoWith;
}

TEST(StrategyInvariants,
     StrategyExecutionInvariantToTickerSectorAndExternalState) {
  const std::vector<SimpleBarSpec> AllBars{
      SimpleBarSpec{"100", "101", "99", "100", 50000},
      SimpleBarSpec{"100", "102", "99", "101", 50000},
      SimpleBarSpec{"101", "103", "100", "102", 50000},
      SimpleBarSpec{"102", "105", "101", "104", 50000},
      SimpleBarSpec{"104", "106", "102", "103", 50000},
      SimpleBarSpec{"103", "104", "94", "95", 50000},
      SimpleBarSpec{"95", "98", "94", "97", 50000},
      SimpleBarSpec{"97", "100", "96", "99", 50000},
  };

  auto BuildEnv = [&](const std::string &SnapId, const std::string &Ticker,
                      const std::string &Sector, std::int32_t StartYmd) {
    InMemoryBarSource BarSrc;
    InMemoryCalendarSource CalSrc;
    const Decimal PriceStep = Decimal::parse("0.01");
    BarSrc.addSecurity(SnapId, Security{Ticker, Sector, 10, PriceStep});

    std::vector<Bar> Bars;
    std::vector<TradingDay> Cal;
    std::int32_t FromYmd = 0;
    std::int32_t ToYmd = 0;
    const std::int64_t BaseMs = moscowDayStartMs(StartYmd);
    for (std::size_t I = 0; I < AllBars.size(); ++I) {
      const std::int64_t OpenMs =
          BaseMs + static_cast<std::int64_t>(I) * kDayMs;
      const std::int32_t Ymd = moscowYmd(OpenMs);
      if (I == 2) {
        FromYmd = Ymd;
      }
      ToYmd = Ymd;
      Cal.push_back(TradingDay{Ymd, true});

      Bar Item;
      Item.TsOpenMs = OpenMs;
      Item.AvailableAtMs = OpenMs + kDayMs;
      Item.Open = Decimal::parse(AllBars[I].Open);
      Item.High = Decimal::parse(AllBars[I].High);
      Item.Low = Decimal::parse(AllBars[I].Low);
      Item.Close = Decimal::parse(AllBars[I].Close);
      Item.Volume = AllBars[I].Volume;
      Item.Value = Item.Close.times(Item.Volume);
      Bars.push_back(Item);
    }
    BarSrc.addSeries(SnapId, Ticker, 86400, std::move(Bars));
    CalSrc.add(SnapId, std::move(Cal));

    const std::vector<Instrument> InstrList{
        Instrument{"A", Ticker, 10, PriceStep}};
    return std::make_shared<const Timeline>(Timeline::loadWithWarmup(
        BarSrc, CalSrc, SnapId, InstrList, Timeframe::D1, FromYmd, ToYmd, 2));
  };

  const auto TlA = BuildEnv("snap-2020", "SBER", "Finance", 20200106);
  const auto TlB = BuildEnv("snap-2025", "GAZP", "Energy", 20250501);
  ASSERT_NE(TlA->yyyymmddOf(0), TlB->yyyymmddOf(0));
  ASSERT_NE(TlA->instruments()[0].Ticker, TlB->instruments()[0].Ticker);

  const std::string Source = R"dsl(
rule "вход"
  when position == 0 and close > sma(close, 2)
  then buy 2 lots market
  because "вход по {close}"

rule "стоп"
  when position > 0
  then sell all stop entry_price * 0.95
  because "стоп от {entry_price}"
)dsl";

  StrategyRunOptions Opts;
  Opts.Source = Source;
  const CostConfig Defaults = CostConfig::defaults();
  const SessionSpec SpecA = makeSessionSpec(TlA, "100000", Defaults);
  const SessionSpec SpecB = makeSessionSpec(TlB, "100000", Defaults);
  const StrategyRunResult ResA = runStrategy(SpecA, TlA, Opts);
  const StrategyRunResult ResB = runStrategy(SpecB, TlB, Opts);
  ASSERT_TRUE(ResA.Compiled);
  ASSERT_TRUE(ResB.Compiled);
  ASSERT_TRUE(ResA.Engine.has_value());
  ASSERT_TRUE(ResB.Engine.has_value());
  ASSERT_FALSE(ResA.Engine->fills().empty());

  EXPECT_EQ(digestOf(*ResA.Engine), digestOf(*ResB.Engine));
  ASSERT_EQ(ResA.Journal.size(), ResB.Journal.size());
  for (std::size_t I = 0; I < ResA.Journal.size(); ++I) {
    EXPECT_TRUE(sameJournalEntry(ResA.Journal[I], ResB.Journal[I]));
  }
  ASSERT_EQ(ResA.CommandResults.size(), ResB.CommandResults.size());
  for (std::size_t I = 0; I < ResA.CommandResults.size(); ++I) {
    const auto &CmdA = ResA.CommandResults[I];
    const auto &CmdB = ResB.CommandResults[I];
    EXPECT_TRUE(sameCommandResult(CmdA, CmdB));
  }
  ASSERT_EQ(ResA.Engine->fills().size(), ResB.Engine->fills().size());
  for (std::size_t I = 0; I < ResA.Engine->fills().size(); ++I) {
    EXPECT_TRUE(sameFill(ResA.Engine->fills()[I], ResB.Engine->fills()[I]));
  }
  ASSERT_EQ(ResA.Engine->orders().size(), ResB.Engine->orders().size());
  for (std::size_t I = 0; I < ResA.Engine->orders().size(); ++I) {
    const Order &Oa = ResA.Engine->orders()[I];
    const Order &Ob = ResB.Engine->orders()[I];
    EXPECT_TRUE(sameOrderExecution(Oa, Ob));
    EXPECT_EQ(Oa.RuleName, Ob.RuleName);
    EXPECT_EQ(Oa.Because, Ob.Because);
  }

  dsl::CompileResult CompA = dsl::compile(Source, {}, 1);
  dsl::CompileResult CompB = dsl::compile(Source, {}, 1);
  ASSERT_TRUE(CompA.Ok && CompB.Ok);
  dsl::Bar DslBarItem{0, 100.0, 102.0, 99.0, 101.0, 5000.0};
  dsl::PortfolioSnapshot DslSnap;
  DslSnap.Cash = 100000.0;
  DslSnap.Equity = 100000.0;
  const dsl::StepOutcome OutA =
      CompA.Instance->step(0, DslBarItem, DslSnap, false);
  const dsl::StepOutcome OutB =
      CompB.Instance->step(0, DslBarItem, DslSnap, false);
  EXPECT_EQ(OutA.Error, OutB.Error);
  EXPECT_EQ(OutA.Intents, OutB.Intents);

  const BenchmarkSeries BenchA{
      Decimal::parse("2500"),
      {DailyClose{TlA->yyyymmddOf(0), Decimal::parse("2550")}}};
  const BenchmarkSeries BenchB{
      Decimal::parse("3000"),
      {DailyClose{TlA->yyyymmddOf(0), Decimal::parse("3300")}}};
  const RuoniaSeries RuoniaA{{DailyRate{20200101, Decimal::parse("6.0")}}};
  const RuoniaSeries RuoniaB{{DailyRate{20200101, Decimal::parse("18.0")}}};
  const Decimal Cash = Decimal::parse("100000");
  const std::uint64_t DigestBefore = digestOf(*ResA.Engine);
  const Report RepA =
      buildStrategyReport(ResA, Cash, *TlA, BenchA, RuoniaA, 20200107);
  const Report RepB =
      buildStrategyReport(ResA, Cash, *TlA, BenchB, RuoniaB, 20200107);
  EXPECT_EQ(digestOf(*ResA.Engine), DigestBefore);
  EXPECT_EQ(RepA.FinalEquity, RepB.FinalEquity);
  EXPECT_NE(RepA.Stats.BenchmarkReturn, RepB.Stats.BenchmarkReturn);
}

} // namespace
} // namespace core
