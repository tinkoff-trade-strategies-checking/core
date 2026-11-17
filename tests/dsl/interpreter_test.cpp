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
  Program program;
  std::vector<ResolvedParam> params;
};

Loaded load(const std::string &source) {
  ParseResult parsed = parse(source);
  if (!parsed.program)
    throw std::runtime_error("не разобралось: " + source);
  CheckResult checked = check(*parsed.program);
  if (checked.level == Level::Error)
    throw std::runtime_error("не прошло проверку: " + source);
  return Loaded{std::move(*parsed.program), std::move(checked.params)};
}

StrategyInstance makeInstance(const std::string &source,
                              const ParamValues &overrides = {},
                              std::size_t instruments = 1) {
  const Loaded loaded = load(source);
  return StrategyInstance(loaded.program, loaded.params, overrides,
                          instruments);
}

// Программа из одного правила.
std::string oneRule(const std::string &when,
                    const std::string &action = "buy 1 lots",
                    const std::string &because = "\"причина\"",
                    const std::string &params = "") {
  return params + "rule \"r\" when " + when + " then " + action + " because " +
         because + "\n";
}

StepBar makeBar(std::int64_t step, const std::string &close, int instrument = 0,
                std::int64_t volume = 100) {
  StepBar bar;
  bar.step = step;
  bar.instrument = instrument;
  bar.open = bar.high = bar.low = bar.close = Decimal::parse(close);
  bar.volumeLots = volume;
  return bar;
}

const Decimal kStep = Decimal::parse("0.01");

using Intents = std::vector<Intent>;

StateView flat(double cash = 1000000) {
  StateView state;
  state.cash = cash;
  state.equity = cash;
  return state;
}

// Прогон по ценам закрытия одной бумаги: onBar + evaluate на каждом баре.
std::vector<Intents>
runCloses(StrategyInstance &strategy, const std::vector<std::string> &closes,
          std::int64_t lotSize = 1,
          const std::function<StateView(std::size_t)> &state = nullptr) {
  std::vector<Intents> out;
  for (std::size_t i = 0; i < closes.size(); ++i) {
    const StepBar bar = makeBar(static_cast<std::int64_t>(i), closes[i]);
    strategy.onBar(0, bar);
    out.push_back(
        strategy.evaluate(0, bar, lotSize, kStep, state ? state(i) : flat()));
  }
  return out;
}

TEST(Interpreter, CrossoverFiresOnce) {
  StrategyInstance strategy =
      makeInstance(oneRule("crossover(sma(close, 2), sma(close, 3))",
                           "buy 1 lots", "\"пересечение\""));
  const auto result = runCloses(strategy, {"10", "9", "8", "9", "11", "12"});
  ASSERT_EQ(result.size(), 6u);
  for (std::size_t i = 0; i < result.size(); ++i) {
    if (i == 4) {
      ASSERT_EQ(result[i].size(), 1u);
      EXPECT_EQ(result[i][0].order.side, Side::Buy);
      EXPECT_EQ(result[i][0].order.lots, 1);
      EXPECT_EQ(result[i][0].order.type, OrderType::Market);
      EXPECT_EQ(result[i][0].ruleName, "r");
      EXPECT_FALSE(result[i][0].cancelAll);
    } else {
      EXPECT_TRUE(result[i].empty()) << "бар " << i;
    }
  }
}

TEST(Interpreter, PercentCashSizing) {
  StrategyInstance strategy = makeInstance(oneRule("true", "buy 95 % cash"));
  const StepBar bar = makeBar(0, "271.35");
  strategy.onBar(0, bar);
  const Intents intents = strategy.evaluate(0, bar, 10, kStep, flat(1000000));
  ASSERT_EQ(intents.size(), 1u);
  EXPECT_EQ(intents[0].order.lots, 350); // 950000 / 2713.5 = 350.1
}

TEST(Interpreter, PercentEquityAndPositionSizing) {
  StrategyInstance strategy = makeInstance(
      "rule \"a\" when true then buy 10 % equity because \"a\"\n"
      "rule \"b\" when true then sell 50 % position because \"b\"\n");
  const StepBar bar = makeBar(0, "100");
  strategy.onBar(0, bar);
  StateView state;
  state.positionLots = 7;
  state.cash = 0;
  state.equity = 100000;
  const Intents intents = strategy.evaluate(0, bar, 1, kStep, state);
  ASSERT_EQ(intents.size(), 2u);
  EXPECT_EQ(intents[0].order.lots, 100); // 10 % от 100000 по 100 за лот
  EXPECT_EQ(intents[1].order.side, Side::Sell);
  EXPECT_EQ(intents[1].order.lots, 3); // floor(3.5)
}

TEST(Interpreter, BecauseFormatting) {
  StrategyInstance strategy =
      makeInstance(oneRule("true", "buy 1 lots", "\"RSI = {rsi(close, 3)}\""));
  const auto result = runCloses(strategy, {"10", "11", "12", "11"});
  EXPECT_EQ(result[2][0].because, "RSI = н/д"); // истории ещё мало
  EXPECT_EQ(result[3][0].because, "RSI = 66,67"); // 66.6666666667

  StrategyInstance withNa =
      makeInstance(oneRule("true", "buy 1 lots", "\"x = {na}\""));
  const StepBar bar = makeBar(0, "1");
  withNa.onBar(0, bar);
  EXPECT_EQ(withNa.evaluate(0, bar, 1, kStep, flat())[0].because, "x = н/д");
}

TEST(Interpreter, BecauseNumbersBooleansAndFractions) {
  StrategyInstance strategy =
      makeInstance(oneRule("true", "buy 1 lots",
                           "\"{close} | {close * 2} | {close > open} | {close "
                           "< open} | {1 / 0} | {{скобки}} | {-close}\""));
  const StepBar bar = makeBar(0, "2.5");
  strategy.onBar(0, bar);
  // open == close: оба сравнения - «нет»; деление на 0 - na
  EXPECT_EQ(strategy.evaluate(0, bar, 1, kStep, flat())[0].because,
            "2,5 | 5 | нет | нет | н/д | {скобки} | -2,5");

  StrategyInstance rounding =
      makeInstance(oneRule("true", "buy 1 lots", "\"{close}\""));
  const StepBar halfUp = makeBar(0, "271.355");
  rounding.onBar(0, halfUp);
  EXPECT_EQ(rounding.evaluate(0, halfUp, 1, kStep, flat())[0].because,
            "271,36");
}

TEST(Interpreter, StopPriceRounding) {
  StrategyInstance strategy = makeInstance(
      oneRule("position > 0", "sell all stop entry_price * 0.95", "\"стоп\""));
  const StepBar bar = makeBar(0, "270");
  strategy.onBar(0, bar);
  StateView state;
  state.positionLots = 4;
  state.entryPrice = 271.54;
  state.cash = 1000;
  state.equity = 2000;
  const Intents intents = strategy.evaluate(0, bar, 10, kStep, state);
  ASSERT_EQ(intents.size(), 1u);
  EXPECT_EQ(intents[0].order.type, OrderType::Stop);
  EXPECT_EQ(intents[0].order.side, Side::Sell);
  EXPECT_EQ(intents[0].order.lots, 4);
  ASSERT_TRUE(intents[0].order.stopPrice.has_value());
  EXPECT_EQ(*intents[0].order.stopPrice,
            Decimal::parse("257.96")); // 257.963 -> HALF_UP
  EXPECT_FALSE(intents[0].order.limitPrice.has_value());
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
  const std::string source =
      oneRule("sma(close, fast) > close", "buy 1 lots", "\"x\"",
              "param fast = 20 range 5..50 step 5\n");
  EXPECT_THROW(makeInstance(source, {{"fast", 60}}), std::invalid_argument);
  EXPECT_THROW(makeInstance(source, {{"fast", 4}}), std::invalid_argument);
  EXPECT_THROW(makeInstance(source, {{"slow", 10}}),
               std::invalid_argument); // неизвестный параметр
  EXPECT_NO_THROW(makeInstance(source, {{"fast", 50}}));
  EXPECT_NO_THROW(makeInstance(source, {{"fast", 5}}));
  EXPECT_THROW(makeInstance(source, {{"fast", std::nan("")}}),
               std::invalid_argument);
}

// INV-1: переопределение записывается в копию программы, и check() выполняется
// заново. Без диапазона у параметра отказ даёт именно повторная проверка.
TEST(Interpreter, OverrideNegativeOffsetIsRefused) {
  const std::string noRange =
      oneRule("close[k] > 0", "buy 1 lots", "\"x\"", "param k = 1\n");
  EXPECT_NO_THROW(makeInstance(noRange));
  EXPECT_THROW(makeInstance(noRange, {{"k", -1}}),
               std::invalid_argument); // E-LA02: чтение будущего
  EXPECT_THROW(makeInstance(noRange, {{"k", 5001}}), std::invalid_argument);
  EXPECT_THROW(makeInstance(noRange, {{"k", 1.5}}), std::invalid_argument);

  const std::string ranged = oneRule("close[k] > 0", "buy 1 lots", "\"x\"",
                                     "param k = 1 range 0..5 step 1\n");
  EXPECT_THROW(makeInstance(ranged, {{"k", -1}}), std::invalid_argument);
  EXPECT_NO_THROW(makeInstance(ranged, {{"k", 0}}));
}

TEST(Interpreter, OverridePeriodZeroOrHugeIsRefused) {
  const std::string source =
      oneRule("sma(close, n) > close", "buy 1 lots", "\"x\"", "param n = 5\n");
  EXPECT_NO_THROW(makeInstance(source));
  EXPECT_THROW(makeInstance(source, {{"n", 0}}), std::invalid_argument);
  EXPECT_THROW(makeInstance(source, {{"n", 100000}}), std::invalid_argument);
  EXPECT_THROW(makeInstance(source, {{"n", 2.5}}), std::invalid_argument);
  EXPECT_THROW(makeInstance(source, {{"n", -3}}), std::invalid_argument);
  EXPECT_NO_THROW(makeInstance(source, {{"n", 5000}}));
}

TEST(Interpreter, ProgramWithErrorsIsRefusedEvenWithoutOverrides) {
  ParseResult parsed = parse(oneRule("close[-1] > close"));
  ASSERT_TRUE(parsed.program.has_value());
  EXPECT_THROW(StrategyInstance(*parsed.program, {}, {}, 1),
               std::invalid_argument);
}

TEST(Interpreter, OverrideChangesBehaviour) {
  const std::string source = oneRule("close > sma(close, n)", "buy 1 lots",
                                     "\"n = {n}\"", "param n = 3\n");
  const std::vector<std::string> closes = {"10", "11", "12", "13"};
  StrategyInstance defaults = makeInstance(source);
  const auto byDefault = runCloses(defaults, closes);
  EXPECT_TRUE(byDefault[1].empty()); // sma(3) ещё na
  ASSERT_EQ(byDefault[3].size(), 1u);
  EXPECT_EQ(byDefault[3][0].because, "n = 3");

  StrategyInstance overridden = makeInstance(source, {{"n", 2}});
  const auto result = runCloses(overridden, closes);
  ASSERT_EQ(result[1].size(), 1u); // sma(2) = 10.5 < 11
  EXPECT_EQ(result[1][0].because, "n = 2");
}

TEST(Interpreter, InstanceOutlivesProgram) {
  std::optional<StrategyInstance> strategy;
  {
    const Loaded loaded = load(oneRule("close > sma(close, 2)"));
    strategy.emplace(loaded.program, loaded.params, ParamValues{}, 1);
  }
  const auto result = runCloses(*strategy, {"10", "12"});
  EXPECT_EQ(result[1].size(), 1u);
}

TEST(Interpreter, IndicatorsUpdateDespiteShortCircuit) {
  StrategyInstance strategy =
      makeInstance("rule \"a\" when false and sma(close, 3) > 0 then buy 1 "
                   "lots because \"a\"\n"
                   "rule \"b\" when position > 0 and sma(close, 3) > 0 then "
                   "buy 1 lots because \"{sma(close, 3)}\"\n");
  const auto result =
      runCloses(strategy, {"10", "20", "30", "40"}, 1, [](std::size_t i) {
        StateView state = flat();
        state.positionLots =
            i == 3 ? 1
                   : 0; // условие правила b доходит до sma только на 4-м баре
        return state;
      });
  for (std::size_t i = 0; i < 3; ++i)
    EXPECT_TRUE(result[i].empty()) << "бар " << i;
  ASSERT_EQ(result[3].size(), 1u);
  EXPECT_EQ(result[3][0].ruleName, "b");
  EXPECT_EQ(result[3][0].because, "30"); // sma последних трёх, а не na
}

TEST(Interpreter, EqualCallsAgree) {
  // sma(close, 2) с литералом и через параметр n = 2 - один и тот же индикатор
  // (один экземпляр).
  StrategyInstance strategy = makeInstance(oneRule(
      "sma(close, 2) == sma(close, n) and sma(close, 2)[1] < sma(close, n)",
      "buy 1 lots", "\"{sma(close, 2)}\"", "param n = 2\n"));
  const auto result = runCloses(strategy, {"10", "12", "14"});
  EXPECT_TRUE(result[0].empty());
  ASSERT_EQ(result[2].size(), 1u); // sma2 = 13; предыдущее 11 < 13
  EXPECT_EQ(result[2][0].because, "13");
}

TEST(Interpreter, SeriesOffsetsAndMissingHistory) {
  StrategyInstance strategy = makeInstance(oneRule(
      "close[2] == 10 and close[1] == 11 and close[0] == 12", "buy 1 lots"));
  const auto result = runCloses(strategy, {"10", "11", "12", "13"});
  EXPECT_TRUE(result[0].empty());
  EXPECT_TRUE(result[1].empty());
  EXPECT_EQ(result[2].size(), 1u);
  EXPECT_TRUE(result[3].empty());
}

TEST(Interpreter, IndicatorOffsetAndChange) {
  // sma(close, 2)[1] - значение индикатора на прошлом баре; change(close, 2) =
  // x - x[2]. change берёт n-е значение назад среди не-na значений (окно из n +
  // 1 значений в indicators.cpp): na на входе не меняет состояние и даёт na.
  // Интерпретатор передаёт na как есть.
  StrategyInstance strategy = makeInstance(oneRule(
      "true", "buy 1 lots", "\"{sma(close, 2)[1]} {change(close, 2)}\""));
  const auto result = runCloses(strategy, {"10", "11", "12", "15"});
  EXPECT_EQ(result[1][0].because, "н/д н/д"); // sma(2)[1] - na (на баре 0 sma
                                              // ещё нет); change: мало значений
  EXPECT_EQ(result[2][0].because, "10,5 2"); // sma2 на баре 1 = 10.5; 12 - 10
  EXPECT_EQ(result[3][0].because, "11,5 4"); // sma2 на баре 2 = 11.5; 15 - 11
}

TEST(Interpreter, NaAndDivisionSemantics) {
  StrategyInstance naCompare =
      makeInstance(oneRule("na > 0 or na < 0 or na == na or na != na"));
  EXPECT_TRUE(runCloses(naCompare, {"1"})[0].empty()); // сравнение с na - false
  StrategyInstance notNa = makeInstance(oneRule("not (na > 0)"));
  EXPECT_EQ(runCloses(notNa, {"1"})[0].size(), 1u);
  StrategyInstance divZero =
      makeInstance(oneRule("close / (close - close) > 0 or close / 0 < 1"));
  EXPECT_TRUE(runCloses(divZero, {"5"})[0]
                  .empty()); // деление на 0 - na, сравнение с na - false
  StrategyInstance sqrtNeg =
      makeInstance(oneRule("sqrt(0 - close) > 0 or sqrt(close) == 2"));
  EXPECT_EQ(runCloses(sqrtNeg, {"4"})[0].size(), 1u);
  StrategyInstance minMax =
      makeInstance(oneRule("min(close, 3) == 3 and max(close, 3) == close and "
                           "abs(0 - close) == close"));
  EXPECT_EQ(runCloses(minMax, {"4"})[0].size(), 1u);
}

TEST(Interpreter, ActionsThatCreateNoOrder) {
  StrategyInstance strategy = makeInstance(
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
  const StepBar bar = makeBar(0, "10");
  strategy.onBar(0, bar);
  const Intents intents = strategy.evaluate(
      0, bar, 1, kStep, flat(5)); // 4,75 денег: меньше одного лота по 10
  ASSERT_EQ(intents.size(), 1u);
  EXPECT_EQ(intents[0].ruleName, "ok");
}

TEST(Interpreter, OrderTypesAndTimeInForce) {
  StrategyInstance strategy = makeInstance(
      "rule \"lim\" when true then buy 2 lots limit close * 0.99 day because "
      "\"a\"\n"
      "rule \"tp\" when true then sell 1 lots take_profit close * 1.05 gtc "
      "because \"b\"\n"
      "rule \"stp\" when true then sell 1 lots stop 90 because \"c\"\n"
      "rule \"mkt\" when true then buy 1 lots market day because \"d\"\n");
  const StepBar bar = makeBar(0, "100");
  strategy.onBar(0, bar);
  const Intents intents = strategy.evaluate(0, bar, 1, kStep, flat());
  ASSERT_EQ(intents.size(), 4u);
  EXPECT_EQ(intents[0].order.type, OrderType::Limit);
  EXPECT_EQ(intents[0].order.limitPrice, Decimal::parse("99"));
  EXPECT_EQ(intents[0].order.tif, TimeInForce::Day);
  EXPECT_EQ(intents[1].order.type, OrderType::TakeProfit);
  EXPECT_EQ(intents[1].order.limitPrice, Decimal::parse("105"));
  EXPECT_EQ(intents[1].order.tif, TimeInForce::Gtc);
  EXPECT_EQ(intents[2].order.type, OrderType::Stop);
  EXPECT_EQ(intents[2].order.stopPrice, Decimal::parse("90"));
  EXPECT_EQ(intents[2].order.tif, TimeInForce::Gtc); // по умолчанию gtc
  EXPECT_EQ(intents[3].order.type, OrderType::Market);
  EXPECT_EQ(intents[3].order.tif, TimeInForce::Day);
}

TEST(Interpreter, CancelAndSellAll) {
  StrategyInstance strategy = makeInstance(
      "rule \"cancel\" when close > open or position > 0 then cancel because "
      "\"снять, цена {close}\"\n"
      "rule \"exit\" when position > 0 then sell all because \"выход\"\n");
  const StepBar bar = makeBar(0, "50");
  strategy.onBar(0, bar);
  StateView state = flat();
  state.positionLots = 5;
  const Intents intents = strategy.evaluate(0, bar, 1, kStep, state);
  ASSERT_EQ(intents.size(), 2u);
  EXPECT_TRUE(intents[0].cancelAll);
  EXPECT_EQ(intents[0].instrument, 0);
  EXPECT_EQ(intents[0].because, "снять, цена 50");
  EXPECT_FALSE(intents[1].cancelAll);
  EXPECT_EQ(intents[1].order.lots, 5);
  EXPECT_EQ(intents[1].order.side, Side::Sell);
}

TEST(Interpreter, PortfolioNamesAndStateDependentIndicator) {
  StrategyInstance strategy = makeInstance(
      oneRule("cash > 100 and equity > 200 and entry_price == 5 and "
              "bars_in_position == 3 and sma(position, 2) >= 1",
              "buy 1 lots", "\"{sma(position, 2)} {position}\""));
  const StepBar b0 = makeBar(0, "1");
  const StepBar b1 = makeBar(1, "1");
  StateView state;
  state.cash = 150;
  state.equity = 250;
  state.entryPrice = 5.0;
  state.barsInPosition = 3;
  state.positionLots = 0;
  strategy.onBar(0, b0);
  EXPECT_TRUE(strategy.evaluate(0, b0, 1, kStep, state).empty());
  state.positionLots = 2;
  strategy.onBar(0, b1);
  const Intents intents = strategy.evaluate(0, b1, 1, kStep, state);
  ASSERT_EQ(intents.size(), 1u);
  EXPECT_EQ(intents[0].because, "1 2");

  StrategyInstance noPosition = makeInstance(
      oneRule("entry_price > 0 or bars_in_position >= 0", "buy 1 lots"));
  noPosition.onBar(0, b0);
  EXPECT_TRUE(noPosition.evaluate(0, b0, 1, kStep, flat())
                  .empty()); // нет позиции: оба na
}

TEST(Interpreter, InstrumentsAreIndependent) {
  StrategyInstance strategy = makeInstance(
      oneRule("close > sma(close, 2)", "buy 1 lots", "\"{close}\""), {}, 2);
  // Бумага 0 растёт, бумага 1 падает; шаги общие, на шаге 1 у бумаги 1 бара
  // нет.
  strategy.onBar(0, makeBar(0, "10", 0));
  strategy.onBar(1, makeBar(0, "20", 1));
  EXPECT_TRUE(
      strategy.evaluate(0, makeBar(0, "10", 0), 1, kStep, flat()).empty());
  EXPECT_TRUE(
      strategy.evaluate(1, makeBar(0, "20", 1), 1, kStep, flat()).empty());
  strategy.onBar(0, makeBar(1, "12", 0));
  const Intents up =
      strategy.evaluate(0, makeBar(1, "12", 0), 1, kStep, flat());
  ASSERT_EQ(up.size(), 1u);
  EXPECT_EQ(up[0].instrument, 0);
  EXPECT_EQ(up[0].order.instrument, 0);
  strategy.onBar(1, makeBar(2, "18", 1));
  EXPECT_TRUE(strategy.evaluate(1, makeBar(2, "18", 1), 1, kStep, flat())
                  .empty()); // 18 < sma(20, 18) = 19
}

TEST(Interpreter, CallOrderIsChecked) {
  StrategyInstance strategy = makeInstance(oneRule("true"), {}, 2);
  const StepBar bar = makeBar(0, "1");
  EXPECT_THROW(strategy.evaluate(0, bar, 1, kStep, flat()),
               std::logic_error); // onBar не было
  strategy.onBar(0, bar);
  EXPECT_THROW(strategy.onBar(0, bar), std::invalid_argument); // шаг не растёт
  EXPECT_THROW(strategy.onBar(1, bar),
               std::invalid_argument); // бар другой бумаги
  EXPECT_THROW(strategy.onBar(2, makeBar(0, "1", 2)), std::out_of_range);
  EXPECT_THROW(strategy.evaluate(0, makeBar(1, "1"), 1, kStep, flat()),
               std::logic_error); // не тот бар
  EXPECT_THROW(strategy.evaluate(0, bar, 0, kStep, flat()),
               std::invalid_argument);
  EXPECT_THROW(strategy.evaluate(0, bar, 1, Decimal(), flat()),
               std::invalid_argument);
  EXPECT_NO_THROW(strategy.evaluate(0, bar, 1, kStep, flat()));
  EXPECT_THROW(strategy.evaluate(0, bar, 1, kStep, flat()),
               std::logic_error); // повторное вычисление
  EXPECT_THROW(makeInstance(oneRule("true"), {}, 0), std::invalid_argument);
}

TEST(Interpreter, AtrAndVolumeSeries) {
  StrategyInstance strategy = makeInstance(
      oneRule("volume == 100 and atr(2) >= 0", "buy 1 lots", "\"{atr(2)}\""));
  // high = low = close: TR = 0, 2, 4; atr(2) по Уайлдеру: (0 + 2) / 2 = 1,
  // затем (1 * 1 + 4) / 2 = 2,5
  const auto result = runCloses(strategy, {"10", "12", "16"});
  EXPECT_TRUE(result[0].empty()); // atr(2) ещё na
  ASSERT_EQ(result[1].size(), 1u);
  EXPECT_EQ(result[1][0].because, "1");
  ASSERT_EQ(result[2].size(), 1u);
  EXPECT_EQ(result[2][0].because, "2,5");
}

TEST(Interpreter, NonFiniteIndicatorOutputIsNa) {
  // Сумма окна sma переполняет double: выход индикатора - na, исключений нет,
  // заявки нет. Литералов с порядком (1e300) в языке нет: 1e8^38 * 10000 =
  // 1e308, сумма двух таких не помещается в double.
  std::string huge = "close";
  for (int i = 1; i < 38; ++i)
    huge += " * close";
  huge += " * 10000";
  const std::string average = "sma(" + huge + ", 2)";
  StrategyInstance strategy = makeInstance(
      oneRule("true", "buy 1 lots limit " + average, "\"{" + average + "}\""));
  for (std::int64_t i = 0; i < 4; ++i) {
    const StepBar bar = makeBar(i, "100000000");
    strategy.onBar(0, bar);
    Intents intents;
    EXPECT_NO_THROW(intents = strategy.evaluate(0, bar, 1, kStep, flat()))
        << "бар " << i;
    EXPECT_TRUE(intents.empty()) << "бар " << i;
  }
  // Не конечное состояние от вызывающего - тоже na.
  StrategyInstance withState = makeInstance(oneRule("cash > 0", "buy 1 lots"));
  const StepBar bar = makeBar(0, "1");
  withState.onBar(0, bar);
  StateView state = flat();
  state.cash = std::nan("");
  EXPECT_TRUE(withState.evaluate(0, bar, 1, kStep, state).empty());
}

TEST(Interpreter, OnBarRequiresEvaluateOfPreviousBar) {
  StrategyInstance strategy = makeInstance(oneRule("sma(position, 2) >= 0"));
  strategy.onBar(0, makeBar(0, "1"));
  EXPECT_THROW(strategy.onBar(0, makeBar(1, "1")),
               std::logic_error); // бар 0 не вычислен
  EXPECT_NO_THROW(strategy.evaluate(0, makeBar(0, "1"), 1, kStep, flat()));
  EXPECT_NO_THROW(strategy.onBar(0, makeBar(1, "1")));
}

TEST(Interpreter, MovedFromInstanceThrowsLogicError) {
  StrategyInstance first = makeInstance(oneRule("true"));
  StrategyInstance second = std::move(first);
  const StepBar bar = makeBar(0, "1");
  EXPECT_THROW(first.onBar(0, bar), std::logic_error);
  EXPECT_THROW(first.evaluate(0, bar, 1, kStep, flat()), std::logic_error);
  second.onBar(0, bar);
  EXPECT_EQ(second.evaluate(0, bar, 1, kStep, flat()).size(), 1u);
}

TEST(Interpreter, DeterministicRepeat) {
  const std::string source = oneRule(
      "crossover(sma(close, 2), sma(close, 3)) or rsi(close, 3) > 50",
      "buy 10 % equity limit close * 0.97", "\"{rsi(close, 3)} {close}\"");
  const std::vector<std::string> closes = {"10", "9",  "8",  "9",
                                           "11", "12", "11", "10.5"};
  StrategyInstance first = makeInstance(source);
  StrategyInstance second = makeInstance(source);
  const auto a = runCloses(first, closes);
  const auto b = runCloses(second, closes);
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    ASSERT_EQ(a[i].size(), b[i].size());
    for (std::size_t j = 0; j < a[i].size(); ++j) {
      EXPECT_EQ(a[i][j].because, b[i][j].because);
      EXPECT_EQ(a[i][j].order.lots, b[i][j].order.lots);
      EXPECT_EQ(a[i][j].order.limitPrice, b[i][j].order.limitPrice);
    }
  }
}

} // namespace
} // namespace core::dsl
