#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "core/engine/digest.hpp"
#include "core/market/date_utils.hpp"
#include "core/report/ruonia.hpp"
#include "core/strategy/runtime_detectors.hpp"
#include "core/strategy/strategy_runner.hpp"
#include "dsl/interpreter.hpp"
#include "support/fixture1.hpp"

namespace core {
namespace {

struct SimpleBar {
  std::string Open;
  std::string High;
  std::string Low;
  std::string Close;
  std::int64_t Volume = 10000;
};

std::shared_ptr<const Timeline>
makeTimeline(const std::vector<SimpleBar> &BarsSpec,
             const std::string &StepStr = "0.01", std::int64_t LotSize = 1,
             const std::vector<SimpleBar> &WarmupSpec = {}) {
  std::vector<Instrument> Instruments{
      Instrument{"A", "TEST", LotSize, Decimal::parse(StepStr)}};
  std::vector<Bar> Bars;
  std::vector<TradingDay> Calendar;
  const std::int64_t BaseMs = moscowDayStartMs(20240201);

  std::int64_t DayIdx = 0;
  auto PushBar = [&](const SimpleBar &S, std::vector<Bar> &Out) {
    const std::int64_t OpenMs = BaseMs + DayIdx * kDayMs;
    ++DayIdx;
    Bar B;
    B.TsOpenMs = OpenMs;
    B.AvailableAtMs = OpenMs + kDayMs;
    B.Open = Decimal::parse(S.Open);
    B.High = Decimal::parse(S.High);
    B.Low = Decimal::parse(S.Low);
    B.Close = Decimal::parse(S.Close);
    B.Volume = S.Volume;
    B.Value = B.Close.times(B.Volume);
    Out.push_back(B);
    Calendar.push_back(TradingDay{moscowYmd(OpenMs), true});
  };

  std::vector<Bar> WarmupBars;
  for (const SimpleBar &S : WarmupSpec) {
    PushBar(S, WarmupBars);
  }
  for (const SimpleBar &S : BarsSpec) {
    PushBar(S, Bars);
  }

  Timeline MainTl = Timeline::build(Instruments, {Bars}, Calendar);
  if (!WarmupBars.empty()) {
    const Timeline Hist = Timeline::build(Instruments, {WarmupBars}, Calendar);
    const auto WarmupCount = static_cast<std::int64_t>(WarmupBars.size());
    MainTl = MainTl.withWarmup(Hist, WarmupCount);
  }
  return std::make_shared<const Timeline>(std::move(MainTl));
}

SessionSpec makeSpec(const std::shared_ptr<const Timeline> &Tl,
                     const std::string &CashStr = "1000000",
                     CostConfig Costs = CostConfig::zero()) {
  SessionSpec Spec;
  Spec.Instruments = Tl->instruments();
  Spec.Costs = Costs;
  Spec.InitialCash = Decimal::parse(CashStr);
  return Spec;
}

TEST(StrategyRunner, ReferenceStrategyWithWarmupAndBecause) {
  const std::string ReferenceSource = R"dsl(
# Эталон платформы: пересечение скользящих средних
param fast = 20 range 5..50 step 5
param slow = 50 range 20..200 step 10
param rsi_n = 14
param rsi_high = 70 range 60..85 step 5
param stop_pct = 5 range 2..10 step 1
param size_pct = 95

rule "вход"
  when crossover(sma(close, fast), sma(close, slow)) and position == 0
  then buy size_pct % cash
  because "SMA({fast}) пересекла SMA({slow}) снизу вверх"

rule "стоп"
  when position > 0
  then sell all stop entry_price * (1 - stop_pct / 100)
  because "защитный стоп {stop_pct}% от цены входа {entry_price}"

rule "перекупленность"
  when position > 0 and rsi(close, rsi_n) > rsi_high
  then sell all
  because "перекупленность, RSI = {rsi(close, rsi_n)}"

rule "выход"
  when position > 0 and crossunder(sma(close, fast), sma(close, slow))
  then sell all
  because "SMA({fast}) опустилась ниже SMA({slow})"
)dsl";

  // 55 баров прогрева: 54 бара по 100 и 1 бар 99, чтобы fast SMA < slow SMA
  // перед шагом 0, а на шаге 0 при росте цены произошёл crossover.
  std::vector<SimpleBar> Warmup;
  for (int I = 0; I < 54; ++I) {
    Warmup.push_back(SimpleBar{"100", "100", "100", "100", 100000});
  }
  Warmup.push_back(SimpleBar{"100", "100", "99", "99", 100000});
  // Торговые бары: рост на шаге 0 -> вход на открытии шага 1 -> стоп на шаге 1
  // -> далее рост.
  std::vector<SimpleBar> Trading{
      SimpleBar{"100", "121", "100", "120", 100000},
      SimpleBar{"120", "125", "119", "124", 100000},
      SimpleBar{"124", "130", "123", "129", 100000},
      SimpleBar{"129", "135", "128", "134", 100000},
      SimpleBar{"134", "140", "133", "139", 100000},
  };

  const auto Tl = makeTimeline(Trading, "0.01", 1, Warmup);
  StrategyRunOptions Opts;
  Opts.Source = ReferenceSource;
  const StrategyRunResult Res = runStrategy(makeSpec(Tl), Tl, Opts);

  ASSERT_TRUE(Res.Compiled);
  EXPECT_FALSE(Res.Invalid);
  EXPECT_FALSE(Res.Cancelled);
  EXPECT_EQ(Res.StaticLookahead, CheckLevel::Ok);
  ASSERT_TRUE(Res.Engine.has_value());
  ASSERT_GE(Res.Engine->orders().size(), 2u);
  EXPECT_EQ(Res.Engine->orders()[0].RuleName, "вход");
  EXPECT_EQ(Res.Engine->orders()[0].Because,
            "SMA(20) пересекла SMA(50) снизу вверх");
  EXPECT_EQ(Res.Engine->orders()[1].RuleName, "стоп");
  EXPECT_NE(Res.Engine->orders()[1].Because.find("защитный стоп 5%"),
            std::string::npos);
  EXPECT_FALSE(Res.Engine->fills().empty());
}

TEST(StrategyRunner, ReplaceAcceptedAndRejectedPreservesBecause) {
  const std::string Source = R"dsl(
rule "лимит"
  when true
  then buy (close - 90) lots limit 50
  because "лимит при close={close}"
)dsl";

  // Шаг 0: close=91 -> 1 лот @ 50 (Accepted).
  // Шаг 1: close=120 -> 30 лотов @ 50 (> cash=1000 -> Rejected, старая Id=0 и
  // Because сохранены). Шаг 2: close=91 -> 1 лот @ 50 == Existing->Spec ->
  // тихая дедупликация, новых команд нет. Шаг 3: close=92 -> 2 лота @ 50 ->
  // ReplaceCmd Accepted, новая заявка с новым Because. Шаг 4: последний шаг,
  // намерения не вычисляются.
  const auto Tl = makeTimeline({
      SimpleBar{"91", "92", "90", "91"},
      SimpleBar{"120", "121", "90", "120"},
      SimpleBar{"91", "92", "90", "91"},
      SimpleBar{"92", "93", "90", "92"},
      SimpleBar{"92", "93", "90", "92"},
  });
  StrategyRunOptions Opts;
  Opts.Source = Source;
  const StrategyRunResult Res = runStrategy(makeSpec(Tl, "1000"), Tl, Opts);

  ASSERT_TRUE(Res.Compiled);
  ASSERT_EQ(Res.Journal.size(), 3u);
  EXPECT_EQ(Res.Journal[0].Step, 0);
  EXPECT_TRUE(std::holds_alternative<PlaceCmd>(Res.Journal[0].Command));
  EXPECT_TRUE(Res.CommandResults[0].Accepted);

  EXPECT_EQ(Res.Journal[1].Step, 1);
  EXPECT_TRUE(std::holds_alternative<ReplaceCmd>(Res.Journal[1].Command));
  EXPECT_FALSE(Res.CommandResults[1].Accepted);
  EXPECT_EQ(Res.CommandResults[1].ReasonCode, Reason::InsufficientCash);

  EXPECT_EQ(Res.Journal[2].Step, 3);
  EXPECT_TRUE(std::holds_alternative<ReplaceCmd>(Res.Journal[2].Command));
  EXPECT_TRUE(Res.CommandResults[2].Accepted);

  ASSERT_TRUE(Res.Engine.has_value());
  const auto &Orders = Res.Engine->orders();
  ASSERT_GE(Orders.size(), 3u);
  // Заявка 0 была сохранена на шаге 1 и заменена только на шаге 3.
  EXPECT_EQ(Orders[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(Orders[0].ReasonCode, Reason::Replaced);
  EXPECT_EQ(Orders[0].ClosedStep, 3);
  EXPECT_EQ(Orders[0].Because, "лимит при close=91");

  // Отклонённая заявка замены на шаге 1 осталась Rejected.
  EXPECT_EQ(Orders[1].Status, OrderStatus::Rejected);
  EXPECT_EQ(Orders[1].Because, "лимит при close=120");

  // Новая активная (закрыта SessionEnd на шаге 4) имеет новый Because.
  EXPECT_EQ(Orders[2].ReasonCode, Reason::SessionEnd);
  EXPECT_EQ(Orders[2].Because, "лимит при close=92");
}

TEST(StrategyRunner, SubStepZeroPriceRejectedByEngine) {
  const std::string Source = R"dsl(
rule "подшаг"
  when true
  then buy 1 lots limit 0.004
  because "цена меньше половины шага"
)dsl";

  const auto Tl = makeTimeline(
      {SimpleBar{"10", "11", "9", "10"}, SimpleBar{"10", "11", "9", "10"}},
      "0.01");
  StrategyRunOptions Opts;
  Opts.Source = Source;
  const StrategyRunResult Res = runStrategy(makeSpec(Tl), Tl, Opts);

  ASSERT_TRUE(Res.Compiled);
  ASSERT_EQ(Res.CommandResults.size(), 1u);
  EXPECT_FALSE(Res.CommandResults[0].Accepted);
  EXPECT_EQ(Res.CommandResults[0].ReasonCode, Reason::InvalidParams);
  EXPECT_EQ(Res.CommandResults[0].Field, "limitPrice");
  ASSERT_TRUE(Res.Engine.has_value());
  ASSERT_EQ(Res.Engine->orders().size(), 1u);
  EXPECT_EQ(Res.Engine->orders()[0].Status, OrderStatus::Rejected);
  EXPECT_EQ(Res.Engine->orders()[0].InvalidField, "limitPrice");
}

TEST(StrategyRunner, CancelWithoutActiveOrdersIsSilentNoOp) {
  const std::string Source = R"dsl(
rule "отмена"
  when true
  then cancel
  because "нечего снимать"
)dsl";

  const auto Tl = makeTimeline(
      {SimpleBar{"10", "11", "9", "10"}, SimpleBar{"10", "11", "9", "10"}});
  StrategyRunOptions Opts;
  Opts.Source = Source;
  const StrategyRunResult Res = runStrategy(makeSpec(Tl), Tl, Opts);

  ASSERT_TRUE(Res.Compiled);
  EXPECT_TRUE(Res.Journal.empty());
  EXPECT_TRUE(Res.CommandResults.empty());
  ASSERT_TRUE(Res.Engine.has_value());
  EXPECT_TRUE(Res.Engine->orders().empty());
}

TEST(StrategyRunner, BuyThenCancelOnSameBarCancelsPlacedOrder) {
  const std::string Source = R"dsl(
rule "вход"
  when true
  then buy 1 lots market
  because "хотим купить"

rule "отмена"
  when true
  then cancel
  because "передумали на том же баре"
)dsl";

  const auto Tl = makeTimeline(
      {SimpleBar{"10", "11", "9", "10"}, SimpleBar{"10", "11", "9", "10"}});
  StrategyRunOptions Opts;
  Opts.Source = Source;
  const StrategyRunResult Res = runStrategy(makeSpec(Tl), Tl, Opts);

  ASSERT_TRUE(Res.Compiled);
  ASSERT_EQ(Res.Journal.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<PlaceCmd>(Res.Journal[0].Command));
  EXPECT_TRUE(std::holds_alternative<CancelCmd>(Res.Journal[1].Command));
  EXPECT_EQ(Res.Journal[1].RuleName, "отмена");
  EXPECT_EQ(Res.Journal[1].Because, "передумали на том же баре");
  EXPECT_TRUE(Res.CommandResults[0].Accepted);
  EXPECT_TRUE(Res.CommandResults[1].Accepted);
  ASSERT_TRUE(Res.Engine.has_value());
  ASSERT_EQ(Res.Engine->orders().size(), 1u);
  EXPECT_EQ(Res.Engine->orders()[0].Status, OrderStatus::Cancelled);
  EXPECT_EQ(Res.Engine->orders()[0].ReasonCode, Reason::CancelledByUser);
  EXPECT_TRUE(Res.Engine->fills().empty());
}

TEST(StrategyRunner, SkippedBarPreservesBarsInPositionAndSkipsStep) {
  auto Fix = testing::fixture1();
  const auto Tl = std::make_shared<const Timeline>(
      Timeline::load(Fix.Bars, Fix.Calendar, Fix.SnapshotId, Fix.Instruments,
                     Timeframe::D1, 20240109, 20240122));
  const std::string Source = R"dsl(
rule "вход"
  when position == 0
  then buy 1 lots market
  because "открываем позицию"

rule "выход"
  when position > 0 and bars_in_position >= 2
  then sell all
  because "закрытие на баре {bars_in_position}"
)dsl";

  StrategyRunOptions Opts;
  Opts.Source = Source;
  const StrategyRunResult Res = runStrategy(makeSpec(Tl), Tl, Opts);
  ASSERT_TRUE(Res.Compiled);
  ASSERT_TRUE(Res.Engine.has_value());

  // На шаге 3 у FIXA (instr=0) bars_in_position == 2, поэтому sell
  // подаётся на шаге 3. У FIXB (instr=1) шаг 3 пропущен -> счётчик остаётся 1,
  // sell подаётся на шаге 4.
  bool FoundSellA = false;
  bool FoundSellB = false;
  for (const Order &Ord : Res.Engine->orders()) {
    if (Ord.Spec.OrderSide == Side::Sell && Ord.Spec.Instrument == 0 &&
        Ord.PlacedStep == 3) {
      FoundSellA = true;
    }
    if (Ord.Spec.OrderSide == Side::Sell && Ord.Spec.Instrument == 1 &&
        Ord.PlacedStep == 4) {
      FoundSellB = true;
    }
    const bool IsSellBOnStep3 = Ord.Spec.OrderSide == Side::Sell &&
                                Ord.Spec.Instrument == 1 && Ord.PlacedStep == 3;
    EXPECT_FALSE(IsSellBOnStep3);
  }
  EXPECT_TRUE(FoundSellA);
  EXPECT_TRUE(FoundSellB);
}

TEST(StrategyRunner, CooperativeCancellationAndTimeBudget) {
  const auto Tl = makeTimeline({
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "99", "100"},
  });
  const std::string Source = R"dsl(
rule "покупка"
  when position == 0
  then buy 1 lots market
  because "покупаем"
)dsl";

  int Checks = 0;
  StrategyRunOptions CancelOpts;
  CancelOpts.Source = Source;
  CancelOpts.ShouldCancel = [&]() { return ++Checks >= 2; };
  const StrategyRunResult CancelledRes =
      runStrategy(makeSpec(Tl), Tl, CancelOpts);
  EXPECT_TRUE(CancelledRes.Compiled);
  EXPECT_TRUE(CancelledRes.Cancelled);
  ASSERT_TRUE(CancelledRes.Engine.has_value());
  EXPECT_LT(CancelledRes.Engine->currentStep(), Tl->totalSteps() - 1);

  StrategyRunOptions BudgetOpts;
  BudgetOpts.Source = Source;
  BudgetOpts.TimeBudget = std::chrono::steady_clock::duration::zero();
  const StrategyRunResult BudgetRes = runStrategy(makeSpec(Tl), Tl, BudgetOpts);
  EXPECT_TRUE(BudgetRes.Compiled);
  EXPECT_TRUE(BudgetRes.Cancelled);
}

TEST(StrategyRunner, IntegrityErrorOnNonIncreasingBarStep) {
  const std::string Source = R"dsl(
rule "покупка"
  when true
  then buy 1 lots market
  because "покупаем"
)dsl";
  dsl::CompileResult Compiled = dsl::compile(Source, {}, 1);
  ASSERT_TRUE(Compiled.Ok);
  ASSERT_NE(Compiled.Instance, nullptr);

  dsl::Bar B;
  B.Step = 1;
  B.Open = B.High = B.Low = B.Close = 100.0;
  B.Volume = 1000.0;
  const dsl::StepOutcome First =
      Compiled.Instance->step(0, B, dsl::PortfolioSnapshot{}, false);
  EXPECT_FALSE(First.Error.has_value());

  const dsl::StepOutcome Second =
      Compiled.Instance->step(0, B, dsl::PortfolioSnapshot{}, false);
  ASSERT_TRUE(Second.Error.has_value());
  EXPECT_EQ(Second.Error->Code, "E-RT01");

  const std::vector<SimpleBar> Bars{
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "99", "100"},
  };
  const auto Tl = makeTimeline(Bars);
  Report Rep;
  const Finding ErrFinding{Second.Error->Code, CheckLevel::Error,
                           Second.Error->Message, B.Step};
  applyRuntimeChecks(Rep, CheckLevel::Ok, *Tl, {}, {}, {ErrFinding});
  EXPECT_EQ(Rep.ReportLevel, CheckLevel::Error);
  ASSERT_EQ(Rep.Findings.size(), 1u);
  EXPECT_EQ(Rep.Findings[0].Code, "E-RT01");
}

TEST(StrategyRunner, SameExecutionEngineAsHuman_INV3) {
  const std::string Source = R"dsl(
rule "вход"
  when position == 0 and close == 100
  then buy 2 lots market
  because "вход"

rule "стоп"
  when position > 0
  then sell all stop 90
  because "защитный стоп"

rule "выход"
  when position > 0 and close >= 105
  then sell all
  because "фиксация прибыли"
)dsl";

  const auto Tl = makeTimeline({
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "102", "99", "101"},
      SimpleBar{"101", "106", "101", "105"},
      SimpleBar{"106", "107", "105", "106"},
      SimpleBar{"106", "107", "105", "106"},
  });
  const SessionSpec Spec = makeSpec(Tl, "100000", CostConfig::defaults());

  StrategyRunOptions Opts;
  Opts.Source = Source;
  const StrategyRunResult AlgoRes = runStrategy(Spec, Tl, Opts);
  ASSERT_TRUE(AlgoRes.Compiled);
  ASSERT_TRUE(AlgoRes.Engine.has_value());
  ASSERT_FALSE(AlgoRes.Engine->fills().empty());

  std::vector<JournalEntry> HumanJournal = AlgoRes.Journal;
  for (JournalEntry &Entry : HumanJournal) {
    Entry.RuleName.clear();
    Entry.Because.clear();
  }
  const ReplayResult HumanRes =
      replaySession(Spec, Tl, HumanJournal, Tl->totalSteps() - 1, 0);

  EXPECT_EQ(digestOf(*AlgoRes.Engine), digestOf(HumanRes.Engine));
  EXPECT_EQ(AlgoRes.Engine->fills().size(), HumanRes.Engine.fills().size());
  EXPECT_EQ(AlgoRes.Engine->portfolio().cash(),
            HumanRes.Engine.portfolio().cash());
}

TEST(StrategyRunner, AlgorithmHasNoNewsOrDateAccess_INV9) {
  const std::vector<SimpleBar> Bars{
      SimpleBar{"100", "101", "99", "100"},
      SimpleBar{"100", "101", "99", "100"},
  };
  const auto Tl = makeTimeline(Bars);
  for (const char *Forbidden : {"news", "date", "time", "secid"}) {
    StrategyRunOptions Opts;
    Opts.Source = std::string("rule \"запрещено\" when ") + Forbidden +
                  " > 0 then buy 1 lots market because \"тест\"";
    const StrategyRunResult Res = runStrategy(makeSpec(Tl), Tl, Opts);
    EXPECT_FALSE(Res.Compiled);
    EXPECT_FALSE(Res.Engine.has_value());
    ASSERT_FALSE(Res.Diagnostics.empty());
    EXPECT_EQ(Res.Diagnostics[0].Code, "E-TYPE");
  }
}

TEST(StrategyRunner, BuildStrategyReportWithRuoniaAndWarnings) {
  // 14 баров с чередующимися гэпами +3% / -2.9% для срабатывания W-RT01.
  std::vector<SimpleBar> Bars;
  for (int I = 0; I < 14; ++I) {
    if (I % 2 == 0) {
      Bars.push_back(SimpleBar{"100", "101", "99", "100"});
    } else {
      Bars.push_back(SimpleBar{"103", "104", "102", "103"});
    }
  }
  const auto Tl = makeTimeline(Bars);
  const SessionSpec Spec = makeSpec(Tl);

  // Добавляем статическое предупреждение W-LA01 + правила с W-RT01.
  const std::string Source = R"dsl(
param n = 2
param lvl = 150
rule "предупреждение"
  when sma(close, n) > lvl
  then cancel
  because "статический уровень"

rule "покупка"
  when position == 0 and close == 100
  then buy 1 lots market
  because "вход перед гэпом"

rule "продажа"
  when position > 0 and close == 103
  then sell all market
  because "выход перед гэпом"
)dsl";
  StrategyRunOptions Opts;
  Opts.Source = Source;
  const StrategyRunResult RunRes = runStrategy(Spec, Tl, Opts);
  ASSERT_TRUE(RunRes.Compiled);
  EXPECT_EQ(RunRes.StaticLookahead, CheckLevel::Warning);

  RuoniaSeries Rates;
  const std::int64_t BaseMs = moscowDayStartMs(20240201);
  const std::int32_t PrevDay = moscowYmd(BaseMs - kDayMs);
  Rates.Rates.push_back(DailyRate{PrevDay, Decimal::parse("15.0")});
  for (std::int64_t Step = 0; Step < Tl->totalSteps(); ++Step) {
    Rates.Rates.push_back(
        DailyRate{Tl->yyyymmddOf(Step), Decimal::parse("15.0")});
  }

  const Report Rep = buildStrategyReport(RunRes, Spec.InitialCash, *Tl,
                                         std::nullopt, Rates, PrevDay);
  EXPECT_EQ(Rep.ReportLevel, CheckLevel::Warning);
  EXPECT_TRUE(Rep.Stats.Sharpe.has_value());
  EXPECT_TRUE(Rep.Stats.Volatility.has_value());

  int Wrt01Count = 0;
  for (const Finding &Item : Rep.Findings) {
    if (Item.Code == "W-RT01") {
      ++Wrt01Count;
    }
  }
  EXPECT_EQ(Wrt01Count, 1);
}

TEST(StrategyRunner, BuildStrategyReportWithTooSmoothAndInvalidRuns) {
  // 64 растущих бара: 31 прибыльный круг -> W-RT02.
  std::vector<SimpleBar> RisingBars;
  for (int I = 0; I < 64; ++I) {
    const std::string Price = std::to_string(100 + I * 2);
    RisingBars.push_back(SimpleBar{Price, Price, Price, Price});
  }
  const auto Tl = makeTimeline(RisingBars);
  const SessionSpec Spec = makeSpec(Tl);

  StrategyRunOptions Opts;
  Opts.Source = R"dsl(
rule "вход"
  when position == 0
  then buy 1 lots market
  because "вход"

rule "выход"
  when position > 0
  then sell all market
  because "выход"
)dsl";
  StrategyRunResult RunRes = runStrategy(Spec, Tl, Opts);
  ASSERT_TRUE(RunRes.Compiled);
  ASSERT_TRUE(RunRes.Engine.has_value());

  const RuoniaSeries Rates = [&]() {
    RuoniaSeries Out;
    const std::int32_t Day0 = moscowYmd(moscowDayStartMs(20240201) - kDayMs);
    Out.Rates.push_back(DailyRate{Day0, Decimal::parse("15.0")});
    for (std::int64_t Step = 0; Step < Tl->totalSteps(); ++Step) {
      Out.Rates.push_back(
          DailyRate{Tl->yyyymmddOf(Step), Decimal::parse("15.0")});
    }
    return Out;
  }();
  const std::int32_t PrevDay = Rates.Rates.front().Yyyymmdd;

  const Report SmoothRep = buildStrategyReport(RunRes, Spec.InitialCash, *Tl,
                                               std::nullopt, Rates, PrevDay);
  EXPECT_EQ(SmoothRep.ReportLevel, CheckLevel::Warning);
  int Wrt02Count = 0;
  for (const Finding &Item : SmoothRep.Findings) {
    if (Item.Code == "W-RT02") {
      ++Wrt02Count;
    }
  }
  EXPECT_EQ(Wrt02Count, 1);

  // При Invalid == true сохраняется E-RT01 без ложных W-RT01/W-RT02.
  RunRes.Invalid = true;
  RunRes.FinalLevel = CheckLevel::Error;
  RunRes.RuntimeFindings = {Finding{"E-RT01", CheckLevel::Error, "сбой", 5}};
  const Report InvalidRep = buildStrategyReport(RunRes, Spec.InitialCash, *Tl,
                                                std::nullopt, Rates, PrevDay);
  EXPECT_EQ(InvalidRep.ReportLevel, CheckLevel::Error);
  ASSERT_EQ(InvalidRep.Findings.size(), 1u);
  EXPECT_EQ(InvalidRep.Findings[0].Code, "E-RT01");

  // Нескомпилированная стратегия (!Engine): FinalEquity == InitialCash.
  StrategyRunOptions BadOpts;
  BadOpts.Source =
      "rule \"r\" when close[-1] > close then cancel because \"bad\"";
  const StrategyRunResult BadRes = runStrategy(Spec, Tl, BadOpts);
  EXPECT_FALSE(BadRes.Compiled);
  EXPECT_FALSE(BadRes.Engine.has_value());
  const Report BadRep = buildStrategyReport(BadRes, Spec.InitialCash, *Tl);
  EXPECT_EQ(BadRep.FinalEquity, Spec.InitialCash);
  EXPECT_EQ(BadRep.ReportLevel, CheckLevel::Error);
}

TEST(StrategyRunner, UpToStepAndOnProgress) {
  std::vector<SimpleBar> Bars;
  for (int I = 0; I < 120; ++I) {
    Bars.push_back(SimpleBar{"100", "102", "99", "101", 10000});
  }
  const auto Tl = makeTimeline(Bars);
  const std::string Source = R"dsl(
rule "покупка"
  when position == 0
  then buy 1 lots market
  because "вход"
)dsl";

  std::vector<std::pair<std::int64_t, std::int64_t>> Progress;
  StrategyRunOptions Opts;
  Opts.Source = Source;
  Opts.UpToStep = 75;
  Opts.OnProgress = [&](std::int64_t Done, std::int64_t Total) {
    Progress.emplace_back(Done, Total);
  };

  const StrategyRunResult Res = runStrategy(makeSpec(Tl), Tl, Opts);
  ASSERT_TRUE(Res.Compiled);
  EXPECT_FALSE(Res.Invalid);
  EXPECT_FALSE(Res.Cancelled);
  ASSERT_TRUE(Res.Engine.has_value());
  EXPECT_EQ(Res.Engine->currentStep(), 75);
  EXPECT_EQ(Res.Engine->equity().size(), 76u);
  ASSERT_EQ(Progress.size(), 2u);
  EXPECT_EQ(Progress[0], (std::pair<std::int64_t, std::int64_t>{50, 76}));
  EXPECT_EQ(Progress[1], (std::pair<std::int64_t, std::int64_t>{76, 76}));

  StrategyRunOptions BadOpts;
  BadOpts.Source = Source;
  BadOpts.UpToStep = 120;
  EXPECT_THROW(runStrategy(makeSpec(Tl), Tl, BadOpts), std::invalid_argument);
}

} // namespace
} // namespace core
