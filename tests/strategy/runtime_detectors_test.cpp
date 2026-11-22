#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/market/date_utils.hpp"
#include "core/strategy/runtime_detectors.hpp"

namespace core {
namespace {

struct BarSpec {
  std::int64_t DayIndex = 0;
  std::string Open;
  std::string Close;
};

Timeline makeSingleTimeline(const std::vector<BarSpec> &Specs) {
  std::vector<Instrument> Instruments{
      Instrument{"A", "TEST", 1, Decimal::parse("0.01")}};
  std::vector<Bar> Bars;
  std::vector<TradingDay> Calendar;
  const std::int64_t BaseMs = moscowDayStartMs(20240102);

  for (const BarSpec &Item : Specs) {
    const std::int64_t OpenMs = BaseMs + Item.DayIndex * kDayMs;
    const Decimal Open = Decimal::parse(Item.Open);
    const Decimal Close = Decimal::parse(Item.Close);
    Bar B;
    B.TsOpenMs = OpenMs;
    B.AvailableAtMs = OpenMs + kDayMs;
    B.Open = Open;
    B.High = std::max(Open, Close) + Decimal::one();
    B.Low = std::min(Open, Close) - Decimal::one();
    B.Close = Close;
    B.Volume = 10000;
    B.Value = Close.times(B.Volume);
    Bars.push_back(B);
    Calendar.push_back(TradingDay{moscowYmd(OpenMs), true});
  }
  return Timeline::build(std::move(Instruments), {Bars}, Calendar);
}

Order makeOrder(std::int64_t Id, std::int64_t PlacedStep, Side OrderSide,
                OrderType Type, const std::string &RuleName, int Instr = 0) {
  Order Ord;
  Ord.Id = Id;
  Ord.PlacedStep = PlacedStep;
  Ord.Spec.Instrument = Instr;
  Ord.Spec.OrderSide = OrderSide;
  Ord.Spec.Type = Type;
  Ord.Spec.Lots = 1;
  Ord.RuleName = RuleName;
  return Ord;
}

Fill makeFill(std::int64_t OrderId, std::int64_t Step, Side OrderSide,
              std::int64_t Lots, int Instr = 0) {
  Fill F;
  F.OrderId = OrderId;
  F.Step = Step;
  F.Instrument = Instr;
  F.OrderSide = OrderSide;
  F.Lots = Lots;
  return F;
}

TEST(RuntimeDetectors, StaticLookaheadIgnoresL001) {
  dsl::Diagnostic Linter;
  Linter.Code = "L001";
  Linter.Severity = dsl::DiagnosticSeverity::Warning;
  EXPECT_EQ(staticLookaheadLevel({Linter}), CheckLevel::Ok);

  dsl::Diagnostic Warn;
  Warn.Code = "W-LA01";
  Warn.Severity = dsl::DiagnosticSeverity::Warning;
  EXPECT_EQ(staticLookaheadLevel({Linter, Warn}), CheckLevel::Warning);

  dsl::Diagnostic Err;
  Err.Code = "E-LA01";
  Err.Severity = dsl::DiagnosticSeverity::Error;
  EXPECT_EQ(staticLookaheadLevel({Linter, Warn, Err}), CheckLevel::Error);

  dsl::Diagnostic Err2;
  Err2.Code = "E-LA02";
  Err2.Severity = dsl::DiagnosticSeverity::Error;
  EXPECT_EQ(staticLookaheadLevel({Err2}), CheckLevel::Error);
}

TEST(RuntimeDetectors, GuessedGapsThresholdAndExactTwoPercent) {
  // Шаги 0..15: чередующиеся пары (PlacedStep -> NextStep) с гэпами.
  std::vector<BarSpec> Specs;
  // 5 пар: exact +2% (100 -> 102), -2% (100 -> 98), +2%, -2%, +2%;
  // и 1 пара 1.99% (100 -> 101.99) — не считается гэпом.
  std::vector<std::pair<std::string, std::string>> Pairs;
  Pairs.emplace_back("100", "102");
  Pairs.emplace_back("100", "98");
  Pairs.emplace_back("100", "102");
  Pairs.emplace_back("100", "98");
  Pairs.emplace_back("100", "102");
  Pairs.emplace_back("100", "101.99");
  for (std::size_t I = 0; I < Pairs.size(); ++I) {
    const auto Step = static_cast<std::int64_t>(I * 2);
    Specs.push_back(BarSpec{Step, "100", Pairs[I].first});
    Specs.push_back(BarSpec{Step + 1, Pairs[I].second, "100"});
  }
  const Timeline Tl = makeSingleTimeline(Specs);

  // 4 угаданных из 4 (< 5) -> не срабатывает.
  std::vector<Order> Orders;
  std::vector<Fill> Fills;
  for (int I = 0; I < 4; ++I) {
    const Side S = (I % 2 == 0) ? Side::Buy : Side::Sell;
    Orders.push_back(makeOrder(I * 2, I * 2, S, OrderType::Market, "r"));
    Fills.push_back(makeFill(I * 2, I * 2 + 1, S, 1));
  }
  EXPECT_FALSE(detectGuessedGaps(Tl, Orders, Fills).has_value());

  // Добавляем 5-й угаданный гэп и 1.99% (игнорируется как не-гэп) -> 5/5 ->
  // W-RT01.
  Orders.push_back(makeOrder(8, 8, Side::Buy, OrderType::Market, "r"));
  Fills.push_back(makeFill(8, 9, Side::Buy, 1));
  Orders.push_back(makeOrder(10, 10, Side::Sell, OrderType::Market, "r"));
  Fills.push_back(makeFill(10, 11, Side::Sell, 1));
  const auto Res5of5 = detectGuessedGaps(Tl, Orders, Fills);
  ASSERT_TRUE(Res5of5.has_value());
  EXPECT_EQ(Res5of5->Code, "W-RT01");
  EXPECT_EQ(Res5of5->Severity, CheckLevel::Warning);
}

TEST(RuntimeDetectors, GuessedGapsNetZeroSkippedBarsAndFilters) {
  // 2 бумаги: у бумаги 0 пропущен шаг 1 (бар 0: close=100, бар 2: open=105),
  // остальные бары с 3 по 16 имеют гэпы; проверяем 5/7 (71% -> нет) и 5/6
  // (83% -> да).
  std::vector<Instrument> Instruments{
      Instrument{"A", "TESTA", 1, Decimal::parse("0.01")},
      Instrument{"B", "TESTB", 1, Decimal::parse("0.01")}};
  std::vector<Bar> BarsA;
  std::vector<Bar> BarsB;
  std::vector<TradingDay> Calendar;
  const std::int64_t BaseMs = moscowDayStartMs(20240102);

  for (int Day = 0; Day < 20; ++Day) {
    const std::int64_t OpenMs = BaseMs + Day * kDayMs;
    Calendar.push_back(TradingDay{moscowYmd(OpenMs), true});
    Bar Bb;
    Bb.TsOpenMs = OpenMs;
    Bb.AvailableAtMs = OpenMs + kDayMs;
    Bb.Open = Bb.High = Bb.Low = Bb.Close = Decimal::parse("100");
    Bb.Volume = 10000;
    Bb.Value = Bb.Close.times(Bb.Volume);
    BarsB.push_back(Bb);

    if (Day == 1) {
      continue; // у A пропуск бара на шаге 1
    }
    Bar Ba;
    Ba.TsOpenMs = OpenMs;
    Ba.AvailableAtMs = OpenMs + kDayMs;
    // После пропуска (Day == 2) и на нечётных шагах >= 5 задаём open = 105
    // (+5%).
    const bool BigGap = (Day == 2) || (Day >= 5 && Day % 2 == 1);
    Ba.Open = Decimal::parse(BigGap ? "105" : "100");
    Ba.High = Decimal::parse("106");
    Ba.Low = Decimal::parse("99");
    Ba.Close = Decimal::parse("100");
    Ba.Volume = 10000;
    Ba.Value = Ba.Close.times(Ba.Volume);
    BarsA.push_back(Ba);
  }
  const Timeline Tl = Timeline::build(Instruments, {BarsA, BarsB}, Calendar);

  // Шаг 0 -> 2 (пропущен шаг 1): buy 5 -> угадан (1/1).
  // Шаги 4->5, 6->7, 8->9, 10->11: buy 1 -> 4 угаданных (итого 5/5).
  // Шаги 12->13, 14->15: sell 1 -> 2 неугаданных (итого 5/7 = 71% -> нет).
  // Шаг 16->17: net_volume == 0 (buy 3 + sell 3) -> исключён из числителя и
  // знаменателя. Шаг 18->19: limit-заявка и заявка без RuleName ->
  // игнорируются.
  std::vector<Order> Orders{
      makeOrder(0, 0, Side::Buy, OrderType::Market, "r"),
      makeOrder(2, 4, Side::Buy, OrderType::Market, "r"),
      makeOrder(4, 6, Side::Buy, OrderType::Market, "r"),
      makeOrder(6, 8, Side::Buy, OrderType::Market, "r"),
      makeOrder(8, 10, Side::Buy, OrderType::Market, "r"),
      makeOrder(10, 12, Side::Sell, OrderType::Market, "r"),
      makeOrder(12, 14, Side::Sell, OrderType::Market, "r"),
      makeOrder(14, 16, Side::Buy, OrderType::Market, "r1"),
      makeOrder(16, 16, Side::Sell, OrderType::Market, "r2"),
      makeOrder(18, 18, Side::Buy, OrderType::Limit, "r"),
      makeOrder(20, 18, Side::Buy, OrderType::Market, ""),
  };
  std::vector<Fill> Fills;
  const auto AddFill = [&](std::int64_t OrdId, std::int64_t Step, Side S,
                           std::int64_t Lots) {
    Fills.push_back(makeFill(OrdId, Step, S, Lots));
  };
  AddFill(0, 2, Side::Buy, 5);
  AddFill(2, 5, Side::Buy, 1);
  AddFill(4, 7, Side::Buy, 1);
  AddFill(6, 9, Side::Buy, 1);
  AddFill(8, 11, Side::Buy, 1);
  AddFill(10, 13, Side::Sell, 1);
  AddFill(12, 15, Side::Sell, 1);
  AddFill(14, 17, Side::Buy, 3);
  AddFill(16, 17, Side::Sell, 3);
  AddFill(18, 19, Side::Buy, 1);
  AddFill(20, 19, Side::Buy, 1);

  EXPECT_FALSE(detectGuessedGaps(Tl, Orders, Fills).has_value());

  // Убираем один неугаданный гэп -> 5/6 (83.3% >= 80%) -> W-RT01.
  Orders.erase(Orders.begin() + 6);
  Fills.erase(Fills.begin() + 6);
  const auto Res5of6 = detectGuessedGaps(Tl, Orders, Fills);
  ASSERT_TRUE(Res5of6.has_value());
  EXPECT_EQ(Res5of6->Code, "W-RT01");
}

TEST(RuntimeDetectors, TooSmoothResultBoundary) {
  auto makeTrips = [](std::size_t Total, std::size_t Wins) {
    std::vector<RoundTrip> Trips;
    for (std::size_t I = 0; I < Total; ++I) {
      RoundTrip T{};
      T.NetPnl = (I < Wins) ? Decimal::fromUnits(10) : Decimal::fromUnits(-10);
      Trips.push_back(T);
    }
    return Trips;
  };

  EXPECT_FALSE(detectTooSmoothResult(makeTrips(29, 29)).has_value());
  // 90.0%
  EXPECT_FALSE(detectTooSmoothResult(makeTrips(30, 27)).has_value());
  // 93.3%
  const auto Res = detectTooSmoothResult(makeTrips(30, 28));
  ASSERT_TRUE(Res.has_value());
  EXPECT_EQ(Res->Code, "W-RT02");
  EXPECT_EQ(Res->Severity, CheckLevel::Warning);
}

TEST(RuntimeDetectors, ApplyRuntimeChecksAggregatesLevelsAndFindings) {
  const Timeline Tl = makeSingleTimeline({
      BarSpec{0, "100", "100"},
      BarSpec{1, "100", "100"},
  });
  Report Rep;
  for (int I = 0; I < 30; ++I) {
    RoundTrip T{};
    T.NetPnl = Decimal::fromUnits(5);
    Rep.RoundTrips.push_back(T);
  }

  applyRuntimeChecks(Rep, CheckLevel::Ok, Tl, {}, {});
  ASSERT_EQ(Rep.Findings.size(), 1u);
  EXPECT_EQ(Rep.Findings[0].Code, "W-RT02");
  EXPECT_EQ(Rep.ReportLevel, CheckLevel::Warning);

  applyRuntimeChecks(
      Rep, CheckLevel::Warning, Tl, {}, {},
      {Finding{"E-RT01", CheckLevel::Error, "нарушение целостности", 1}});
  EXPECT_EQ(Rep.ReportLevel, CheckLevel::Error);
  bool HasIntegrity = false;
  for (const Finding &Item : Rep.Findings) {
    if (Item.Code == "E-RT01") {
      HasIntegrity = true;
    }
  }
  EXPECT_TRUE(HasIntegrity);
}

} // namespace
} // namespace core
