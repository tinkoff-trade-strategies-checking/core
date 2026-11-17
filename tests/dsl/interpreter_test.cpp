#include "core/dsl/interpreter.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/dsl/checker.hpp"
#include "core/dsl/parser.hpp"

namespace core::dsl {
namespace {

struct Loaded {
  dsl::Program Program;
  std::vector<ResolvedParam> Params;
};

Loaded load(const std::string &Source) {
  ParseResult Parsed = parse(Source);
  if (!Parsed.Ast)
    throw std::runtime_error("не разобралось: " + Source);
  CheckResult Checked = check(*Parsed.Ast);
  if (Checked.Severity == Level::Error)
    throw std::runtime_error("не прошло проверку: " + Source);
  return Loaded{std::move(*Parsed.Ast), std::move(Checked.Params)};
}

StrategyInstance makeInstance(const std::string &Source,
                              const ParamValues &Overrides = {},
                              std::size_t Instruments = 1) {
  const Loaded Loaded = load(Source);
  return StrategyInstance(Loaded.Program, Loaded.Params, Overrides,
                          Instruments);
}

// Программа из одного правила.
std::string oneRule(const std::string &When,
                    const std::string &Action = "buy 1 lots",
                    const std::string &Because = "\"причина\"",
                    const std::string &Params = "") {
  return Params + "rule \"r\" when " + When + " then " + Action + " because " +
         Because + "\n";
}

StepBar makeBar(std::int64_t Step, const std::string &Close, int Instrument = 0,
                std::int64_t Volume = 100) {
  StepBar Bar;
  Bar.Step = Step;
  Bar.Instrument = Instrument;
  Bar.Open = Bar.High = Bar.Low = Bar.Close = Decimal::parse(Close);
  Bar.VolumeLots = Volume;
  return Bar;
}

const Decimal kStep = Decimal::parse("0.01");

using IntentList = std::vector<Intent>;

StateView flat(double Cash = 1000000) {
  StateView State;
  State.Cash = Cash;
  State.Equity = Cash;
  return State;
}

// Прогон по ценам закрытия одной бумаги: onBar + evaluate на каждом баре.
std::vector<IntentList>
runCloses(StrategyInstance &Strategy, const std::vector<std::string> &Closes,
          std::int64_t LotSize = 1,
          const std::function<StateView(std::size_t)> &State = nullptr) {
  std::vector<IntentList> Out;
  for (std::size_t I = 0; I < Closes.size(); ++I) {
    const StepBar Bar = makeBar(static_cast<std::int64_t>(I), Closes[I]);
    Strategy.onBar(0, Bar);
    Out.push_back(
        Strategy.evaluate(0, Bar, LotSize, kStep, State ? State(I) : flat()));
  }
  return Out;
}

TEST(Interpreter, CrossoverFiresOnce) {
  StrategyInstance Strategy =
      makeInstance(oneRule("crossover(sma(close, 2), sma(close, 3))",
                           "buy 1 lots", "\"пересечение\""));
  const auto Result = runCloses(Strategy, {"10", "9", "8", "9", "11", "12"});
  ASSERT_EQ(Result.size(), 6u);
  for (std::size_t I = 0; I < Result.size(); ++I) {
    if (I == 4) {
      ASSERT_EQ(Result[I].size(), 1u);
      EXPECT_EQ(Result[I][0].Order.OrderSide, Side::Buy);
      EXPECT_EQ(Result[I][0].Order.Lots, 1);
      EXPECT_EQ(Result[I][0].Order.Type, OrderType::Market);
      EXPECT_EQ(Result[I][0].RuleName, "r");
      EXPECT_FALSE(Result[I][0].CancelAll);
    } else {
      EXPECT_TRUE(Result[I].empty()) << "бар " << I;
    }
  }
}

TEST(Interpreter, PercentCashSizing) {
  StrategyInstance Strategy = makeInstance(oneRule("true", "buy 95 % cash"));
  const StepBar Bar = makeBar(0, "271.35");
  Strategy.onBar(0, Bar);
  const IntentList Intents =
      Strategy.evaluate(0, Bar, 10, kStep, flat(1000000));
  ASSERT_EQ(Intents.size(), 1u);
  EXPECT_EQ(Intents[0].Order.Lots, 350); // 950000 / 2713.5 = 350.1
}

TEST(Interpreter, PercentEquityAndPositionSizing) {
  StrategyInstance Strategy = makeInstance(
      "rule \"a\" when true then buy 10 % equity because \"a\"\n"
      "rule \"b\" when true then sell 50 % position because \"b\"\n");
  const StepBar Bar = makeBar(0, "100");
  Strategy.onBar(0, Bar);
  StateView State;
  State.PositionLots = 7;
  State.Cash = 0;
  State.Equity = 100000;
  const IntentList Intents = Strategy.evaluate(0, Bar, 1, kStep, State);
  ASSERT_EQ(Intents.size(), 2u);
  EXPECT_EQ(Intents[0].Order.Lots, 100); // 10 % от 100000 по 100 за лот
  EXPECT_EQ(Intents[1].Order.OrderSide, Side::Sell);
  EXPECT_EQ(Intents[1].Order.Lots, 3); // floor(3.5)
}

TEST(Interpreter, BecauseFormatting) {
  StrategyInstance Strategy =
      makeInstance(oneRule("true", "buy 1 lots", "\"RSI = {rsi(close, 3)}\""));
  const auto Result = runCloses(Strategy, {"10", "11", "12", "11"});
  EXPECT_EQ(Result[2][0].Because, "RSI = н/д"); // истории ещё мало
  EXPECT_EQ(Result[3][0].Because, "RSI = 66,67"); // 66.6666666667

  StrategyInstance WithNa =
      makeInstance(oneRule("true", "buy 1 lots", "\"x = {na}\""));
  const StepBar Bar = makeBar(0, "1");
  WithNa.onBar(0, Bar);
  EXPECT_EQ(WithNa.evaluate(0, Bar, 1, kStep, flat())[0].Because, "x = н/д");
}

TEST(Interpreter, BecauseNumbersBooleansAndFractions) {
  StrategyInstance Strategy =
      makeInstance(oneRule("true", "buy 1 lots",
                           "\"{close} | {close * 2} | {close > open} | {close "
                           "< open} | {1 / 0} | {{скобки}} | {-close}\""));
  const StepBar Bar = makeBar(0, "2.5");
  Strategy.onBar(0, Bar);
  // open == close: оба сравнения - «нет»; деление на 0 - na
  EXPECT_EQ(Strategy.evaluate(0, Bar, 1, kStep, flat())[0].Because,
            "2,5 | 5 | нет | нет | н/д | {скобки} | -2,5");

  StrategyInstance Rounding =
      makeInstance(oneRule("true", "buy 1 lots", "\"{close}\""));
  const StepBar HalfUp = makeBar(0, "271.355");
  Rounding.onBar(0, HalfUp);
  EXPECT_EQ(Rounding.evaluate(0, HalfUp, 1, kStep, flat())[0].Because,
            "271,36");
}

TEST(Interpreter, StopPriceRounding) {
  StrategyInstance Strategy = makeInstance(
      oneRule("position > 0", "sell all stop entry_price * 0.95", "\"стоп\""));
  const StepBar Bar = makeBar(0, "270");
  Strategy.onBar(0, Bar);
  StateView State;
  State.PositionLots = 4;
  State.EntryPrice = 271.54;
  State.Cash = 1000;
  State.Equity = 2000;
  const IntentList Intents = Strategy.evaluate(0, Bar, 10, kStep, State);
  ASSERT_EQ(Intents.size(), 1u);
  EXPECT_EQ(Intents[0].Order.Type, OrderType::Stop);
  EXPECT_EQ(Intents[0].Order.OrderSide, Side::Sell);
  EXPECT_EQ(Intents[0].Order.Lots, 4);
  ASSERT_TRUE(Intents[0].Order.StopPrice.has_value());
  EXPECT_EQ(*Intents[0].Order.StopPrice,
            Decimal::parse("257.96")); // 257.963 -> HALF_UP
  EXPECT_FALSE(Intents[0].Order.LimitPrice.has_value());
}

TEST(Interpreter, ToDecimalPrice) {
  EXPECT_EQ(toDecimalPrice(0.125, kStep),
            Decimal::parse("0.13")); // ровно половина - от нуля
  EXPECT_EQ(toDecimalPrice(10.025, Decimal::parse("0.05")),
            Decimal::parse("10.05"));
  EXPECT_EQ(toDecimalPrice(257.963, kStep), Decimal::parse("257.96"));
  EXPECT_THROW(toDecimalPrice(std::nan(""), kStep), std::invalid_argument);
  EXPECT_THROW(toDecimalPrice(1e300, kStep), DecimalOverflow);
}

TEST(Interpreter, ParamOverrideOutOfRange) {
  const std::string Source =
      oneRule("sma(close, fast) > close", "buy 1 lots", "\"x\"",
              "param fast = 20 range 5..50 step 5\n");
  EXPECT_THROW(makeInstance(Source, {{"fast", 60}}), std::invalid_argument);
  EXPECT_THROW(makeInstance(Source, {{"fast", 4}}), std::invalid_argument);
  EXPECT_THROW(makeInstance(Source, {{"slow", 10}}),
               std::invalid_argument); // неизвестный параметр
  EXPECT_NO_THROW(makeInstance(Source, {{"fast", 50}}));
  EXPECT_NO_THROW(makeInstance(Source, {{"fast", 5}}));
  EXPECT_THROW(makeInstance(Source, {{"fast", std::nan("")}}),
               std::invalid_argument);
}

// INV-1: переопределение записывается в копию программы, и check() выполняется
// заново. Без диапазона у параметра отказ даёт именно повторная проверка.
TEST(Interpreter, OverrideNegativeOffsetIsRefused) {
  const std::string NoRange =
      oneRule("close[k] > 0", "buy 1 lots", "\"x\"", "param k = 1\n");
  EXPECT_NO_THROW(makeInstance(NoRange));
  EXPECT_THROW(makeInstance(NoRange, {{"k", -1}}),
               std::invalid_argument); // E-LA02: чтение будущего
  EXPECT_THROW(makeInstance(NoRange, {{"k", 5001}}), std::invalid_argument);
  EXPECT_THROW(makeInstance(NoRange, {{"k", 1.5}}), std::invalid_argument);

  const std::string Ranged = oneRule("close[k] > 0", "buy 1 lots", "\"x\"",
                                     "param k = 1 range 0..5 step 1\n");
  EXPECT_THROW(makeInstance(Ranged, {{"k", -1}}), std::invalid_argument);
  EXPECT_NO_THROW(makeInstance(Ranged, {{"k", 0}}));
}

TEST(Interpreter, OverridePeriodZeroOrHugeIsRefused) {
  const std::string Source =
      oneRule("sma(close, n) > close", "buy 1 lots", "\"x\"", "param n = 5\n");
  EXPECT_NO_THROW(makeInstance(Source));
  EXPECT_THROW(makeInstance(Source, {{"n", 0}}), std::invalid_argument);
  EXPECT_THROW(makeInstance(Source, {{"n", 100000}}), std::invalid_argument);
  EXPECT_THROW(makeInstance(Source, {{"n", 2.5}}), std::invalid_argument);
  EXPECT_THROW(makeInstance(Source, {{"n", -3}}), std::invalid_argument);
  EXPECT_NO_THROW(makeInstance(Source, {{"n", 5000}}));
}

TEST(Interpreter, ProgramWithErrorsIsRefusedEvenWithoutOverrides) {
  ParseResult Parsed = parse(oneRule("close[-1] > close"));
  ASSERT_TRUE(Parsed.Ast.has_value());
  EXPECT_THROW(StrategyInstance(*Parsed.Ast, {}, {}, 1), std::invalid_argument);
}

TEST(Interpreter, OverrideChangesBehaviour) {
  const std::string Source = oneRule("close > sma(close, n)", "buy 1 lots",
                                     "\"n = {n}\"", "param n = 3\n");
  const std::vector<std::string> Closes = {"10", "11", "12", "13"};
  StrategyInstance Defaults = makeInstance(Source);
  const auto ByDefault = runCloses(Defaults, Closes);
  EXPECT_TRUE(ByDefault[1].empty()); // sma(3) ещё na
  ASSERT_EQ(ByDefault[3].size(), 1u);
  EXPECT_EQ(ByDefault[3][0].Because, "n = 3");

  StrategyInstance Overridden = makeInstance(Source, {{"n", 2}});
  const auto Result = runCloses(Overridden, Closes);
  ASSERT_EQ(Result[1].size(), 1u); // sma(2) = 10.5 < 11
  EXPECT_EQ(Result[1][0].Because, "n = 2");
}

TEST(Interpreter, InstanceOutlivesProgram) {
  std::optional<StrategyInstance> Strategy;
  {
    const Loaded Loaded = load(oneRule("close > sma(close, 2)"));
    Strategy.emplace(Loaded.Program, Loaded.Params, ParamValues{}, 1);
  }
  const auto Result = runCloses(*Strategy, {"10", "12"});
  EXPECT_EQ(Result[1].size(), 1u);
}

TEST(Interpreter, IndicatorsUpdateDespiteShortCircuit) {
  StrategyInstance Strategy =
      makeInstance("rule \"a\" when false and sma(close, 3) > 0 then buy 1 "
                   "lots because \"a\"\n"
                   "rule \"b\" when position > 0 and sma(close, 3) > 0 then "
                   "buy 1 lots because \"{sma(close, 3)}\"\n");
  const auto Result =
      runCloses(Strategy, {"10", "20", "30", "40"}, 1, [](std::size_t I) {
        StateView State = flat();
        State.PositionLots =
            I == 3 ? 1
                   : 0; // условие правила b доходит до sma только на 4-м баре
        return State;
      });
  for (std::size_t I = 0; I < 3; ++I)
    EXPECT_TRUE(Result[I].empty()) << "бар " << I;
  ASSERT_EQ(Result[3].size(), 1u);
  EXPECT_EQ(Result[3][0].RuleName, "b");
  EXPECT_EQ(Result[3][0].Because, "30"); // sma последних трёх, а не na
}

TEST(Interpreter, EqualCallsAgree) {
  // sma(close, 2) с литералом и через параметр n = 2 - один и тот же индикатор
  // (один экземпляр).
  StrategyInstance Strategy = makeInstance(oneRule(
      "sma(close, 2) == sma(close, n) and sma(close, 2)[1] < sma(close, n)",
      "buy 1 lots", "\"{sma(close, 2)}\"", "param n = 2\n"));
  const auto Result = runCloses(Strategy, {"10", "12", "14"});
  EXPECT_TRUE(Result[0].empty());
  ASSERT_EQ(Result[2].size(), 1u); // sma2 = 13; предыдущее 11 < 13
  EXPECT_EQ(Result[2][0].Because, "13");
}

TEST(Interpreter, SeriesOffsetsAndMissingHistory) {
  StrategyInstance Strategy = makeInstance(oneRule(
      "close[2] == 10 and close[1] == 11 and close[0] == 12", "buy 1 lots"));
  const auto Result = runCloses(Strategy, {"10", "11", "12", "13"});
  EXPECT_TRUE(Result[0].empty());
  EXPECT_TRUE(Result[1].empty());
  EXPECT_EQ(Result[2].size(), 1u);
  EXPECT_TRUE(Result[3].empty());
}

TEST(Interpreter, IndicatorOffsetAndChange) {
  // sma(close, 2)[1] - значение индикатора на прошлом баре; change(close, 2) =
  // x - x[2]. change берёт n-е значение назад среди не-na значений (окно из n +
  // 1 значений в indicators.cpp): na на входе не меняет состояние и даёт na.
  // Интерпретатор передаёт na как есть.
  StrategyInstance Strategy = makeInstance(oneRule(
      "true", "buy 1 lots", "\"{sma(close, 2)[1]} {change(close, 2)}\""));
  const auto Result = runCloses(Strategy, {"10", "11", "12", "15"});
  EXPECT_EQ(Result[1][0].Because, "н/д н/д"); // sma(2)[1] - na (на баре 0 sma
                                              // ещё нет); change: мало значений
  EXPECT_EQ(Result[2][0].Because, "10,5 2"); // sma2 на баре 1 = 10.5; 12 - 10
  EXPECT_EQ(Result[3][0].Because, "11,5 4"); // sma2 на баре 2 = 11.5; 15 - 11
}

TEST(Interpreter, NaAndDivisionSemantics) {
  StrategyInstance NaCompare =
      makeInstance(oneRule("na > 0 or na < 0 or na == na or na != na"));
  EXPECT_TRUE(runCloses(NaCompare, {"1"})[0].empty()); // сравнение с na - false
  StrategyInstance NotNa = makeInstance(oneRule("not (na > 0)"));
  EXPECT_EQ(runCloses(NotNa, {"1"})[0].size(), 1u);
  StrategyInstance DivZero =
      makeInstance(oneRule("close / (close - close) > 0 or close / 0 < 1"));
  EXPECT_TRUE(runCloses(DivZero, {"5"})[0]
                  .empty()); // деление на 0 - na, сравнение с na - false
  StrategyInstance SqrtNeg =
      makeInstance(oneRule("sqrt(0 - close) > 0 or sqrt(close) == 2"));
  EXPECT_EQ(runCloses(SqrtNeg, {"4"})[0].size(), 1u);
  StrategyInstance MinMax =
      makeInstance(oneRule("min(close, 3) == 3 and max(close, 3) == close and "
                           "abs(0 - close) == close"));
  EXPECT_EQ(runCloses(MinMax, {"4"})[0].size(), 1u);
}

TEST(Interpreter, ActionsThatCreateNoOrder) {
  StrategyInstance Strategy = makeInstance(
      "rule \"fraction\" when true then buy 0.5 lots because \"a\"\n"
      "rule \"naSize\" when true then buy na lots because \"b\"\n"
      "rule \"naPrice\" when true then buy 1 lots limit na because \"c\"\n"
      "rule \"negPrice\" when true then buy 1 lots limit close - 100 because "
      "\"d\"\n"
      "rule \"zeroPrice\" when true then buy 1 lots limit 0 because \"e\"\n"
      "rule \"roundsToZero\" when true then buy 1 lots limit 0.004 because "
      "\"f\"\n"
      "rule \"sellAllFlat\" when true then sell all because \"g\"\n"
      "rule \"cashTooSmall\" when true then buy 95 % cash because \"h\"\n"
      "rule \"ok\" when true then buy 1 lots because \"i\"\n");
  const StepBar Bar = makeBar(0, "10");
  Strategy.onBar(0, Bar);
  const IntentList Intents = Strategy.evaluate(
      0, Bar, 1, kStep, flat(5)); // 4,75 денег: меньше одного лота по 10
  ASSERT_EQ(Intents.size(), 1u);
  EXPECT_EQ(Intents[0].RuleName, "ok");
}

TEST(Interpreter, OrderTypesAndTimeInForce) {
  StrategyInstance Strategy = makeInstance(
      "rule \"lim\" when true then buy 2 lots limit close * 0.99 day because "
      "\"a\"\n"
      "rule \"tp\" when true then sell 1 lots take_profit close * 1.05 gtc "
      "because \"b\"\n"
      "rule \"stp\" when true then sell 1 lots stop 90 because \"c\"\n"
      "rule \"mkt\" when true then buy 1 lots market day because \"d\"\n");
  const StepBar Bar = makeBar(0, "100");
  Strategy.onBar(0, Bar);
  const IntentList Intents = Strategy.evaluate(0, Bar, 1, kStep, flat());
  ASSERT_EQ(Intents.size(), 4u);
  EXPECT_EQ(Intents[0].Order.Type, OrderType::Limit);
  EXPECT_EQ(Intents[0].Order.LimitPrice, Decimal::parse("99"));
  EXPECT_EQ(Intents[0].Order.Tif, TimeInForce::Day);
  EXPECT_EQ(Intents[1].Order.Type, OrderType::TakeProfit);
  EXPECT_EQ(Intents[1].Order.LimitPrice, Decimal::parse("105"));
  EXPECT_EQ(Intents[1].Order.Tif, TimeInForce::Gtc);
  EXPECT_EQ(Intents[2].Order.Type, OrderType::Stop);
  EXPECT_EQ(Intents[2].Order.StopPrice, Decimal::parse("90"));
  EXPECT_EQ(Intents[2].Order.Tif, TimeInForce::Gtc); // по умолчанию gtc
  EXPECT_EQ(Intents[3].Order.Type, OrderType::Market);
  EXPECT_EQ(Intents[3].Order.Tif, TimeInForce::Day);
}

TEST(Interpreter, CancelAndSellAll) {
  StrategyInstance Strategy = makeInstance(
      "rule \"cancel\" when close > open or position > 0 then cancel because "
      "\"снять, цена {close}\"\n"
      "rule \"exit\" when position > 0 then sell all because \"выход\"\n");
  const StepBar Bar = makeBar(0, "50");
  Strategy.onBar(0, Bar);
  StateView State = flat();
  State.PositionLots = 5;
  const IntentList Intents = Strategy.evaluate(0, Bar, 1, kStep, State);
  ASSERT_EQ(Intents.size(), 2u);
  EXPECT_TRUE(Intents[0].CancelAll);
  EXPECT_EQ(Intents[0].Instrument, 0);
  EXPECT_EQ(Intents[0].Because, "снять, цена 50");
  EXPECT_FALSE(Intents[1].CancelAll);
  EXPECT_EQ(Intents[1].Order.Lots, 5);
  EXPECT_EQ(Intents[1].Order.OrderSide, Side::Sell);
}

TEST(Interpreter, PortfolioNamesAndStateDependentIndicator) {
  StrategyInstance Strategy = makeInstance(
      oneRule("cash > 100 and equity > 200 and entry_price == 5 and "
              "bars_in_position == 3 and sma(position, 2) >= 1",
              "buy 1 lots", "\"{sma(position, 2)} {position}\""));
  const StepBar B0 = makeBar(0, "1");
  const StepBar B1 = makeBar(1, "1");
  StateView State;
  State.Cash = 150;
  State.Equity = 250;
  State.EntryPrice = 5.0;
  State.BarsInPosition = 3;
  State.PositionLots = 0;
  Strategy.onBar(0, B0);
  EXPECT_TRUE(Strategy.evaluate(0, B0, 1, kStep, State).empty());
  State.PositionLots = 2;
  Strategy.onBar(0, B1);
  const IntentList Intents = Strategy.evaluate(0, B1, 1, kStep, State);
  ASSERT_EQ(Intents.size(), 1u);
  EXPECT_EQ(Intents[0].Because, "1 2");

  StrategyInstance NoPosition = makeInstance(
      oneRule("entry_price > 0 or bars_in_position >= 0", "buy 1 lots"));
  NoPosition.onBar(0, B0);
  EXPECT_TRUE(NoPosition.evaluate(0, B0, 1, kStep, flat())
                  .empty()); // нет позиции: оба na
}

TEST(Interpreter, InstrumentsAreIndependent) {
  StrategyInstance Strategy = makeInstance(
      oneRule("close > sma(close, 2)", "buy 1 lots", "\"{close}\""), {}, 2);
  // Бумага 0 растёт, бумага 1 падает; шаги общие, на шаге 1 у бумаги 1 бара
  // нет.
  Strategy.onBar(0, makeBar(0, "10", 0));
  Strategy.onBar(1, makeBar(0, "20", 1));
  EXPECT_TRUE(
      Strategy.evaluate(0, makeBar(0, "10", 0), 1, kStep, flat()).empty());
  EXPECT_TRUE(
      Strategy.evaluate(1, makeBar(0, "20", 1), 1, kStep, flat()).empty());
  Strategy.onBar(0, makeBar(1, "12", 0));
  const IntentList Up =
      Strategy.evaluate(0, makeBar(1, "12", 0), 1, kStep, flat());
  ASSERT_EQ(Up.size(), 1u);
  EXPECT_EQ(Up[0].Instrument, 0);
  EXPECT_EQ(Up[0].Order.Instrument, 0);
  Strategy.onBar(1, makeBar(2, "18", 1));
  EXPECT_TRUE(Strategy.evaluate(1, makeBar(2, "18", 1), 1, kStep, flat())
                  .empty()); // 18 < sma(20, 18) = 19
}

TEST(Interpreter, CallOrderIsChecked) {
  StrategyInstance Strategy = makeInstance(oneRule("true"), {}, 2);
  const StepBar Bar = makeBar(0, "1");
  EXPECT_THROW(Strategy.evaluate(0, Bar, 1, kStep, flat()),
               std::logic_error); // onBar не было
  Strategy.onBar(0, Bar);
  EXPECT_THROW(Strategy.onBar(0, Bar), std::invalid_argument); // шаг не растёт
  EXPECT_THROW(Strategy.onBar(1, Bar),
               std::invalid_argument); // бар другой бумаги
  EXPECT_THROW(Strategy.onBar(2, makeBar(0, "1", 2)), std::out_of_range);
  EXPECT_THROW(Strategy.evaluate(0, makeBar(1, "1"), 1, kStep, flat()),
               std::logic_error); // не тот бар
  EXPECT_THROW(Strategy.evaluate(0, Bar, 0, kStep, flat()),
               std::invalid_argument);
  EXPECT_THROW(Strategy.evaluate(0, Bar, 1, Decimal(), flat()),
               std::invalid_argument);
  EXPECT_NO_THROW(Strategy.evaluate(0, Bar, 1, kStep, flat()));
  EXPECT_THROW(Strategy.evaluate(0, Bar, 1, kStep, flat()),
               std::logic_error); // повторное вычисление
  EXPECT_THROW(makeInstance(oneRule("true"), {}, 0), std::invalid_argument);
}

TEST(Interpreter, AtrAndVolumeSeries) {
  StrategyInstance Strategy = makeInstance(
      oneRule("volume == 100 and atr(2) >= 0", "buy 1 lots", "\"{atr(2)}\""));
  // high = low = close: TR = 0, 2, 4; atr(2) по Уайлдеру: (0 + 2) / 2 = 1,
  // затем (1 * 1 + 4) / 2 = 2,5
  const auto Result = runCloses(Strategy, {"10", "12", "16"});
  EXPECT_TRUE(Result[0].empty()); // atr(2) ещё na
  ASSERT_EQ(Result[1].size(), 1u);
  EXPECT_EQ(Result[1][0].Because, "1");
  ASSERT_EQ(Result[2].size(), 1u);
  EXPECT_EQ(Result[2][0].Because, "2,5");
}

TEST(Interpreter, NonFiniteIndicatorOutputIsNa) {
  // Сумма окна sma переполняет double: выход индикатора - na, исключений нет,
  // заявки нет. Литералов с порядком (1e300) в языке нет: 1e8^38 * 10000 =
  // 1e308, сумма двух таких не помещается в double.
  std::string Huge = "close";
  for (int I = 1; I < 38; ++I)
    Huge += " * close";
  Huge += " * 10000";
  const std::string Average = "sma(" + Huge + ", 2)";
  StrategyInstance Strategy = makeInstance(
      oneRule("true", "buy 1 lots limit " + Average, "\"{" + Average + "}\""));
  for (std::int64_t I = 0; I < 4; ++I) {
    const StepBar Bar = makeBar(I, "100000000");
    Strategy.onBar(0, Bar);
    IntentList Intents;
    EXPECT_NO_THROW(Intents = Strategy.evaluate(0, Bar, 1, kStep, flat()))
        << "бар " << I;
    EXPECT_TRUE(Intents.empty()) << "бар " << I;
  }
  // Не конечное состояние от вызывающего - тоже na.
  StrategyInstance WithState = makeInstance(oneRule("cash > 0", "buy 1 lots"));
  const StepBar Bar = makeBar(0, "1");
  WithState.onBar(0, Bar);
  StateView State = flat();
  State.Cash = std::nan("");
  EXPECT_TRUE(WithState.evaluate(0, Bar, 1, kStep, State).empty());
}

TEST(Interpreter, OnBarRequiresEvaluateOfPreviousBar) {
  StrategyInstance Strategy = makeInstance(oneRule("sma(position, 2) >= 0"));
  Strategy.onBar(0, makeBar(0, "1"));
  EXPECT_THROW(Strategy.onBar(0, makeBar(1, "1")),
               std::logic_error); // бар 0 не вычислен
  EXPECT_NO_THROW(Strategy.evaluate(0, makeBar(0, "1"), 1, kStep, flat()));
  EXPECT_NO_THROW(Strategy.onBar(0, makeBar(1, "1")));
}

TEST(Interpreter, MovedFromInstanceThrowsLogicError) {
  StrategyInstance First = makeInstance(oneRule("true"));
  StrategyInstance Second = std::move(First);
  const StepBar Bar = makeBar(0, "1");
  EXPECT_THROW(First.onBar(0, Bar), std::logic_error);
  EXPECT_THROW(First.evaluate(0, Bar, 1, kStep, flat()), std::logic_error);
  Second.onBar(0, Bar);
  EXPECT_EQ(Second.evaluate(0, Bar, 1, kStep, flat()).size(), 1u);
}

TEST(Interpreter, DeterministicRepeat) {
  const std::string Source = oneRule(
      "crossover(sma(close, 2), sma(close, 3)) or rsi(close, 3) > 50",
      "buy 10 % equity limit close * 0.97", "\"{rsi(close, 3)} {close}\"");
  const std::vector<std::string> Closes = {"10", "9",  "8",  "9",
                                           "11", "12", "11", "10.5"};
  StrategyInstance First = makeInstance(Source);
  StrategyInstance Second = makeInstance(Source);
  const auto A = runCloses(First, Closes);
  const auto B = runCloses(Second, Closes);
  ASSERT_EQ(A.size(), B.size());
  for (std::size_t I = 0; I < A.size(); ++I) {
    ASSERT_EQ(A[I].size(), B[I].size());
    for (std::size_t J = 0; J < A[I].size(); ++J) {
      EXPECT_EQ(A[I][J].Because, B[I][J].Because);
      EXPECT_EQ(A[I][J].Order.Lots, B[I][J].Order.Lots);
      EXPECT_EQ(A[I][J].Order.LimitPrice, B[I][J].Order.LimitPrice);
    }
  }
}

} // namespace
} // namespace core::dsl
