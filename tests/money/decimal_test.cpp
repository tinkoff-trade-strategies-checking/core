#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/money/decimal.hpp"

using core::Decimal;
using core::DecimalOverflow;
using core::Int128;
using core::RoundingMode;

namespace {

Decimal D(const char* text) { return Decimal::parse(text); }

constexpr RoundingMode kAllModes[] = {RoundingMode::UP,        RoundingMode::DOWN,
                                      RoundingMode::CEILING,   RoundingMode::FLOOR,
                                      RoundingMode::HALF_UP,   RoundingMode::HALF_DOWN,
                                      RoundingMode::HALF_EVEN};

// ---- INV-6: из double Decimal не строится ---------------------------------------------
static_assert(!std::is_constructible_v<Decimal, double>);
static_assert(!std::is_constructible_v<Decimal, float>);
static_assert(!std::is_constructible_v<Decimal, long double>);
static_assert(!std::is_convertible_v<double, Decimal>);
template <class T>
concept TimesCallable = requires(Decimal d, T t) { d.times(t); };
template <class T>
concept FromUnitsCallable = requires(T t) { Decimal::fromUnits(t); };
static_assert(!TimesCallable<double> && !TimesCallable<float>);
static_assert(!FromUnitsCallable<double> && !FromUnitsCallable<long double>);
static_assert(TimesCallable<std::int64_t> && FromUnitsCallable<std::int64_t>);

// ---- Независимая эталонная реализация округления (только для малых операндов) ---------
// n / d, d > 0, на Int128 через floor-деление; режимы по определению java.math.RoundingMode.
Int128 floorDiv(Int128 n, Int128 d) {
  Int128 q = n / d;
  if ((n % d != 0) && (n < 0)) --q;
  return q;
}

Int128 refRound(Int128 n, Int128 d, RoundingMode mode) {
  const Int128 fl = floorDiv(n, d);
  const Int128 rem = n - fl * d;  // 0 <= rem < d
  if (rem == 0) return fl;
  const Int128 ce = fl + 1;
  const Int128 up = n > 0 ? ce : fl;    // от нуля
  const Int128 down = n > 0 ? fl : ce;  // к нулю
  switch (mode) {
    case RoundingMode::UP: return up;
    case RoundingMode::DOWN: return down;
    case RoundingMode::CEILING: return ce;
    case RoundingMode::FLOOR: return fl;
    default: break;
  }
  const Int128 twice = rem * 2;
  if (twice > d) return ce;
  if (twice < d) return fl;
  switch (mode) {
    case RoundingMode::HALF_UP: return up;
    case RoundingMode::HALF_DOWN: return down;
    default: return (fl % 2 == 0) ? fl : ce;  // HALF_EVEN
  }
}

Int128 pow10(int k) {
  Int128 p = 1;
  while (k-- > 0) p *= 10;
  return p;
}

}  // namespace

// ================================ Разбор и печать ======================================
TEST(DecimalString, ParseAndCanonicalForm) {
  EXPECT_EQ(D("123.45").toString(), "123.45");
  EXPECT_EQ(D("-0.000000001").toString(), "-0.000000001");
  EXPECT_EQ(D("0").toString(), "0");
  EXPECT_EQ(D("-0").toString(), "0");
  EXPECT_EQ(D("5.0").toString(), "5");
  EXPECT_EQ(D("007.500").toString(), "7.5");
  EXPECT_EQ(D("0.000000001").raw(), 1);
  EXPECT_EQ(D("1").raw(), 1000000000);
  EXPECT_EQ(D("123.45").toString(2), "123.45");
  EXPECT_EQ(D("5").toString(2), "5.00");
  EXPECT_EQ(D("-0.5").toString(9), "-0.500000000");
  EXPECT_EQ(Decimal().toString(2), "0.00");
}

TEST(DecimalString, RejectsMalformedInput) {
  for (const char* bad : {"", "-", ".", "1.", ".5", "+1", "1e5", " 1", "1 ", "1.2.3", "--1",
                          "0.0000000001", "abc", "1,5", "1.-5", "- 1"}) {
    EXPECT_THROW(Decimal::parse(bad), std::invalid_argument) << bad;
    EXPECT_FALSE(Decimal::tryParse(bad).has_value()) << bad;
  }
  EXPECT_TRUE(Decimal::tryParse("1.5").has_value());
}

TEST(DecimalString, RangeBoundaryIsInt128Max) {
  const char* kMax = "170141183460469231731687303715.884105727";  // 2^127 - 1 единиц 10^-9
  const Decimal max = D(kMax);
  EXPECT_EQ(max.raw(), Decimal::kMaxRaw);
  EXPECT_EQ(max.toString(), kMax);
  EXPECT_EQ((-max).toString(), std::string("-") + kMax);
  EXPECT_THROW(Decimal::parse("170141183460469231731687303715.884105728"), DecimalOverflow);
  EXPECT_THROW(Decimal::parse("-170141183460469231731687303715.884105728"), DecimalOverflow);
  EXPECT_THROW(Decimal::parse("999999999999999999999999999999999999999"), DecimalOverflow);
  EXPECT_FALSE(Decimal::tryParse("170141183460469231731687303716").has_value());
}

// ================================ Арифметика ===========================================
TEST(DecimalArithmetic, ExactAddSubtractCompare) {
  EXPECT_EQ((D("0.1") + D("0.2")).toString(), "0.3");  // на double было бы 0.30000000000000004
  EXPECT_EQ((D("0.3") - D("0.1")).toString(), "0.2");
  EXPECT_EQ((D("1") - D("1.000000001")).toString(), "-0.000000001");
  EXPECT_EQ((-D("2.5")).toString(), "-2.5");
  EXPECT_EQ(D("-2.5").abs().toString(), "2.5");
  Decimal acc = D("10");
  acc += D("0.01");
  acc -= D("0.02");
  EXPECT_EQ(acc.toString(), "9.99");
  EXPECT_LT(D("-1"), D("0"));
  EXPECT_GT(D("1.000000001"), D("1"));
  EXPECT_EQ(D("1.50"), D("1.5"));
  EXPECT_NE(D("1.5"), D("1.6"));
  EXPECT_TRUE(Decimal().isZero());
  EXPECT_EQ(D("-3").sign(), -1);
  EXPECT_EQ(D("3").sign(), 1);
  EXPECT_EQ(Decimal::zero().sign(), 0);
}

TEST(DecimalArithmetic, TimesInteger) {
  EXPECT_EQ(D("100.16").times(70).toString(), "7011.2");
  EXPECT_EQ(D("0.000000001").times(-3).toString(), "-0.000000003");
  EXPECT_EQ(Decimal::fromUnits(1).times(0), Decimal());
}

TEST(DecimalArithmetic, OverflowThrows) {
  const Decimal max = Decimal::fromRaw(Decimal::kMaxRaw);
  EXPECT_THROW(max + D("0.000000001"), DecimalOverflow);
  EXPECT_THROW((-max) - D("0.000000001"), DecimalOverflow);
  EXPECT_THROW(max.times(2), DecimalOverflow);
  EXPECT_THROW(max.mul(D("2"), RoundingMode::DOWN), DecimalOverflow);
  EXPECT_THROW(max.div(D("0.5"), RoundingMode::DOWN), DecimalOverflow);
  EXPECT_THROW(Decimal::fromRaw(-Decimal::kMaxRaw - 1), DecimalOverflow);
  const Decimal big = Decimal::fromUnits(std::numeric_limits<std::int64_t>::max());
  EXPECT_THROW(big.mul(big, RoundingMode::HALF_UP), DecimalOverflow);
  EXPECT_EQ((max - max), Decimal());
  EXPECT_EQ(max.mul(D("1"), RoundingMode::DOWN), max);
}

TEST(DecimalArithmetic, DivisionByZeroThrows) {
  EXPECT_THROW(D("1").div(Decimal(), RoundingMode::HALF_UP), std::domain_error);
  EXPECT_THROW(Decimal().div(Decimal(), RoundingMode::HALF_UP), std::domain_error);
  EXPECT_THROW(D("1").mulDiv(D("2"), Decimal(), 2, RoundingMode::HALF_UP), std::domain_error);
}

// ================================ Округление ===========================================
namespace {
struct RoundCase {
  const char* in;
  const char* expected[7];  // по порядку kAllModes
};
}  // namespace

TEST(DecimalRounding, EveryModeToWholeUnits) {
  // Таблица из документации java.math.RoundingMode.
  const RoundCase cases[] = {
      //           UP    DOWN   CEIL   FLOOR  H_UP   H_DOWN H_EVEN
      {"5.5",  {"6",  "5",  "6",  "5",  "6",  "5",  "6"}},
      {"2.5",  {"3",  "2",  "3",  "2",  "3",  "2",  "2"}},
      {"1.6",  {"2",  "1",  "2",  "1",  "2",  "2",  "2"}},
      {"1.1",  {"2",  "1",  "2",  "1",  "1",  "1",  "1"}},
      {"1.0",  {"1",  "1",  "1",  "1",  "1",  "1",  "1"}},
      {"0",    {"0",  "0",  "0",  "0",  "0",  "0",  "0"}},
      {"-1.0", {"-1", "-1", "-1", "-1", "-1", "-1", "-1"}},
      {"-1.1", {"-2", "-1", "-1", "-2", "-1", "-1", "-1"}},
      {"-1.6", {"-2", "-1", "-1", "-2", "-2", "-2", "-2"}},
      {"-2.5", {"-3", "-2", "-2", "-3", "-3", "-2", "-2"}},
      {"-5.5", {"-6", "-5", "-5", "-6", "-6", "-5", "-6"}},
      {"0.5",  {"1",  "0",  "1",  "0",  "1",  "0",  "0"}},
      {"-0.5", {"-1", "0",  "0",  "-1", "-1", "0",  "0"}},
      {"0.4",  {"1",  "0",  "1",  "0",  "0",  "0",  "0"}},
      {"-0.4", {"-1", "0",  "0",  "-1", "0",  "0",  "0"}},
  };
  for (const auto& c : cases) {
    for (std::size_t i = 0; i < 7; ++i) {
      EXPECT_EQ(D(c.in).round(0, kAllModes[i]).toString(), c.expected[i])
          << c.in << " mode#" << i;
    }
  }
}

TEST(DecimalRounding, ToKopecksAndOtherScales) {
  EXPECT_EQ(D("3.5056").round(2, RoundingMode::CEILING).toString(2), "3.51");
  EXPECT_EQ(D("3.5001").round(2, RoundingMode::CEILING).toString(2), "3.51");
  EXPECT_EQ(D("3.50").round(2, RoundingMode::CEILING).toString(2), "3.50");  // ровно - не округляем
  EXPECT_EQ(D("0.005").round(2, RoundingMode::HALF_UP).toString(2), "0.01");
  EXPECT_EQ(D("-0.005").round(2, RoundingMode::HALF_UP).toString(2), "-0.01");  // от нуля
  EXPECT_EQ(D("0.004999999").round(2, RoundingMode::HALF_UP).toString(2), "0.00");
  EXPECT_EQ(D("0.015").round(2, RoundingMode::HALF_EVEN).toString(2), "0.02");
  EXPECT_EQ(D("0.025").round(2, RoundingMode::HALF_EVEN).toString(2), "0.02");
  EXPECT_EQ(D("-0.025").round(2, RoundingMode::HALF_DOWN).toString(2), "-0.02");
  EXPECT_EQ(D("-7.777777777").round(6, RoundingMode::FLOOR).toString(),
            "-7.777778");
  // scale 9 - без изменений
  EXPECT_EQ(D("1.123456789").round(9, RoundingMode::UNNECESSARY).toString(), "1.123456789");
}

TEST(DecimalRounding, UnnecessaryThrowsOnlyWhenRoundingNeeded) {
  EXPECT_EQ(D("1.20").round(1, RoundingMode::UNNECESSARY).toString(), "1.2");
  EXPECT_THROW(D("1.25").round(1, RoundingMode::UNNECESSARY), std::domain_error);
  EXPECT_THROW(D("1").div(D("3"), RoundingMode::UNNECESSARY), std::domain_error);
  EXPECT_EQ(D("1").div(D("4"), RoundingMode::UNNECESSARY).toString(), "0.25");
  EXPECT_THROW(D("1.01").roundToStep(D("0.05"), RoundingMode::UNNECESSARY), std::domain_error);
  EXPECT_EQ(D("1.05").roundToStep(D("0.05"), RoundingMode::UNNECESSARY).toString(), "1.05");
}

TEST(DecimalRounding, InvalidScaleThrows) {
  EXPECT_THROW(D("1").round(-1, RoundingMode::DOWN), std::invalid_argument);
  EXPECT_THROW(D("1").round(10, RoundingMode::DOWN), std::invalid_argument);
  EXPECT_THROW(D("1").mul(D("1"), 10, RoundingMode::DOWN), std::invalid_argument);
  EXPECT_THROW(D("1").toString(10), std::invalid_argument);
}

TEST(DecimalRounding, ToPriceStep) {
  // decisions.md, п. 1-2: покупка - CEILING, продажа - FLOOR, DSL и слепой масштаб - HALF_UP.
  const Decimal step = D("0.01");
  EXPECT_EQ(D("100.1550375").roundToStep(step, RoundingMode::CEILING).toString(2), "100.16");
  EXPECT_EQ(D("100.1550375").roundToStep(step, RoundingMode::FLOOR).toString(2), "100.15");
  EXPECT_EQ(D("100.1550375").roundToStep(step, RoundingMode::HALF_UP).toString(2), "100.16");
  EXPECT_EQ(D("100.15").roundToStep(step, RoundingMode::CEILING).toString(2), "100.15");
  EXPECT_EQ(D("99.9049625").roundToStep(step, RoundingMode::FLOOR).toString(2), "99.90");
  // Шаг 0.05, ровно половина шага.
  const Decimal s5 = D("0.05");
  EXPECT_EQ(D("100.125").roundToStep(s5, RoundingMode::HALF_UP).toString(2), "100.15");
  EXPECT_EQ(D("100.125").roundToStep(s5, RoundingMode::HALF_DOWN).toString(2), "100.10");
  EXPECT_EQ(D("100.125").roundToStep(s5, RoundingMode::HALF_EVEN).toString(2), "100.10");  // 2002 - чётное
  EXPECT_EQ(D("100.175").roundToStep(s5, RoundingMode::HALF_EVEN).toString(2), "100.20");  // 2003 -> 2004
  EXPECT_EQ(D("-100.125").roundToStep(s5, RoundingMode::HALF_UP).toString(2), "-100.15");
  EXPECT_EQ(D("-100.125").roundToStep(s5, RoundingMode::FLOOR).toString(2), "-100.15");
  EXPECT_EQ(D("-100.125").roundToStep(s5, RoundingMode::CEILING).toString(2), "-100.10");
  EXPECT_EQ(D("-100.121").roundToStep(s5, RoundingMode::UP).toString(2), "-100.15");
  EXPECT_EQ(D("-100.121").roundToStep(s5, RoundingMode::DOWN).toString(2), "-100.10");
  // Шаг, не являющийся степенью десяти и крупнее единицы.
  EXPECT_EQ(D("17").roundToStep(D("5"), RoundingMode::HALF_UP).toString(), "15");
  EXPECT_EQ(D("17.5").roundToStep(D("5"), RoundingMode::HALF_UP).toString(), "20");
  EXPECT_EQ(D("0.000000007").roundToStep(D("0.000000005"), RoundingMode::CEILING).toString(),
            "0.00000001");
  EXPECT_EQ(D("12.345678912").roundToStep(D("0.000000001"), RoundingMode::FLOOR).toString(),
            "12.345678912");
  EXPECT_THROW(D("1").roundToStep(Decimal(), RoundingMode::DOWN), std::invalid_argument);
  EXPECT_THROW(D("1").roundToStep(D("-0.01"), RoundingMode::DOWN), std::invalid_argument);
}

TEST(DecimalRounding, MultiplyAndDivideEveryMode) {
  // 1.000000001^2 = 1.000000002000000001 -> девять знаков.
  const Decimal x = D("1.000000001");
  EXPECT_EQ(x.mul(x, RoundingMode::DOWN).toString(), "1.000000002");
  EXPECT_EQ(x.mul(x, RoundingMode::UP).toString(), "1.000000003");
  EXPECT_EQ(x.mul(x, RoundingMode::CEILING).toString(), "1.000000003");
  EXPECT_EQ(x.mul(x, RoundingMode::FLOOR).toString(), "1.000000002");
  EXPECT_EQ(x.mul(x, RoundingMode::HALF_UP).toString(), "1.000000002");
  EXPECT_EQ((-x).mul(x, RoundingMode::CEILING).toString(), "-1.000000002");
  EXPECT_EQ((-x).mul(x, RoundingMode::FLOOR).toString(), "-1.000000003");
  EXPECT_EQ((-x).mul(-x, RoundingMode::UP).toString(), "1.000000003");
  // Ровно половина единицы хранения: 0.000000001 * 0.5, 0.000000003 * 0.5.
  const Decimal half = D("0.5");
  const struct { const char* a; const char* expected[7]; } halves[] = {
      {"0.000000001", {"0.000000001", "0", "0.000000001", "0", "0.000000001", "0", "0"}},
      {"0.000000003", {"0.000000002", "0.000000001", "0.000000002", "0.000000001", "0.000000002",
                       "0.000000001", "0.000000002"}},
      {"-0.000000001", {"-0.000000001", "0", "0", "-0.000000001", "-0.000000001", "0", "0"}},
  };
  for (const auto& h : halves) {
    for (std::size_t i = 0; i < 7; ++i) {
      EXPECT_EQ(D(h.a).mul(half, kAllModes[i]).toString(), h.expected[i]) << h.a << " mode#" << i;
    }
  }
  // Деление: 1/3, 2/3, -2/3.
  const Decimal one = D("1"), two = D("2"), three = D("3");
  EXPECT_EQ(one.div(three, RoundingMode::DOWN).toString(), "0.333333333");
  EXPECT_EQ(one.div(three, RoundingMode::UP).toString(), "0.333333334");
  EXPECT_EQ(one.div(three, RoundingMode::HALF_UP).toString(), "0.333333333");
  EXPECT_EQ(two.div(three, RoundingMode::HALF_UP).toString(), "0.666666667");
  EXPECT_EQ(two.div(three, RoundingMode::HALF_EVEN).toString(), "0.666666667");
  EXPECT_EQ((-two).div(three, RoundingMode::FLOOR).toString(), "-0.666666667");
  EXPECT_EQ((-two).div(three, RoundingMode::CEILING).toString(), "-0.666666666");
  EXPECT_EQ(two.div(-three, RoundingMode::HALF_DOWN).toString(), "-0.666666667");
  EXPECT_EQ(D("0.000000001").div(two, RoundingMode::HALF_UP).toString(), "0.000000001");
  EXPECT_EQ(D("0.000000001").div(two, RoundingMode::HALF_DOWN).toString(), "0");
  // Прямое округление до знака без двойного округления: 0.0149 * 1 -> копейки.
  EXPECT_EQ(D("0.0149").mul(one, 2, RoundingMode::HALF_UP).toString(2), "0.01");
  EXPECT_EQ(D("0.0149").mul(one, 2, RoundingMode::CEILING).toString(2), "0.02");
  // Умножение на ноль и ноль в числителе.
  EXPECT_EQ(Decimal().div(three, RoundingMode::UP), Decimal());
  EXPECT_EQ(three.mul(Decimal(), RoundingMode::UP), Decimal());
}

TEST(DecimalRounding, MulDivBlindScaleSingleStep) {
  // decisions.md, п. 8: показанная цена = цена * 100 / P0, 6 знаков HALF_UP.
  const Decimal price = D("253.456789"), p0 = D("250.12");
  EXPECT_EQ(price.mulDiv(D("100"), p0, 6, RoundingMode::HALF_UP).toString(), "101.334075");
  EXPECT_EQ(price.mulDiv(D("100"), p0, 9, RoundingMode::HALF_UP).toString(), "101.334075244");
  // Обратно: цена = показанная * P0 / 100, затем шаг цены HALF_UP.
  const Decimal shown = D("101.334075");
  EXPECT_EQ(shown.mulDiv(p0, D("100"), 9, RoundingMode::HALF_UP).roundToStep(D("0.01"), RoundingMode::HALF_UP).toString(2),
            "253.46");
  EXPECT_EQ(D("100").mulDiv(D("100"), D("100"), 6, RoundingMode::HALF_UP), D("100"));
}

TEST(DecimalRounding, WideIntermediateMatchesPythonReference) {
  // 256-битный путь: a*b не помещается в 128 бит. Эталон посчитан Python (Fraction).
  const Decimal a = D("123456789012345678901.123456789");
  const Decimal b = D("98765432109876543210.987654321");
  const Decimal c = D("11111111111111111111.5");
  EXPECT_EQ(a.mulDiv(b, c, 9, RoundingMode::DOWN).toString(), "1097393680233196156996.268861563");
  EXPECT_EQ(a.mulDiv(b, c, 9, RoundingMode::UP).toString(), "1097393680233196156996.268861564");
  EXPECT_EQ(a.mulDiv(b, c, 9, RoundingMode::HALF_UP).toString(), "1097393680233196156996.268861563");
  EXPECT_EQ(a.mulDiv(b, c, 2, RoundingMode::DOWN).toString(), "1097393680233196156996.26");
  EXPECT_EQ(a.mulDiv(b, c, 2, RoundingMode::HALF_UP).toString(), "1097393680233196156996.27");
  EXPECT_EQ((-a).mulDiv(b, c, 6, RoundingMode::DOWN).toString(), "-1097393680233196156996.268861");
  EXPECT_EQ((-a).mulDiv(b, c, 6, RoundingMode::UP).toString(), "-1097393680233196156996.268862");
  EXPECT_EQ((-a).mulDiv(b, c, 6, RoundingMode::HALF_UP).toString(), "-1097393680233196156996.268862");
  // Результат не помещается -> переполнение, а не молчаливое усечение.
  EXPECT_THROW(a.mulDiv(b, D("0.000000001"), 9, RoundingMode::DOWN), DecimalOverflow);
}

// ================================ Эталонная сделка =====================================
TEST(DecimalReference, TradeCycleByRules) {
  // Бумага: шаг цены 0.01, лот 10. Open = 100.03; спред/проскальзывание дают множитель.
  const Decimal open = D("100.03"), step = D("0.01");
  const Decimal commissionRate = D("0.0005"), exchangeRate = D("0.0001");
  const std::int64_t lots = 7, lotSize = 10;

  // Покупка: 100.03 * 1.00125 = 100.1550375 -> шаг CEILING = 100.16.
  const Decimal buyPrice = open.mul(D("1.00125"), RoundingMode::CEILING).roundToStep(step, RoundingMode::CEILING);
  EXPECT_EQ(buyPrice.toString(2), "100.16");
  // Сумма = 100.16 * 7 * 10 = 7011.20 (HALF_UP до копейки, здесь точно).
  const Decimal amount = buyPrice.times(lots * lotSize).round(2, RoundingMode::HALF_UP);
  EXPECT_EQ(amount.toString(2), "7011.20");
  // Комиссия 3.5056 -> 3.51, сбор 0.70112 -> 0.71 (каждая отдельно, CEILING).
  const Decimal commission = amount.mul(commissionRate, 2, RoundingMode::CEILING);
  const Decimal fee = amount.mul(exchangeRate, 2, RoundingMode::CEILING);
  EXPECT_EQ(commission.toString(2), "3.51");
  EXPECT_EQ(fee.toString(2), "0.71");
  // Денежный остаток: 100000.00 - 7011.20 - 3.51 - 0.71 = 92984.58, кратно копейке.
  Decimal cash = D("100000");
  cash -= amount;
  cash -= commission;
  cash -= fee;
  EXPECT_EQ(cash.toString(2), "92984.58");
  EXPECT_EQ(cash, cash.round(2, RoundingMode::UNNECESSARY));

  // Продажа: 100.03 * 0.99875 = 99.9049625 -> шаг FLOOR = 99.90 (не в пользу трейдера).
  const Decimal sellPrice = open.mul(D("0.99875"), RoundingMode::FLOOR).roundToStep(step, RoundingMode::FLOOR);
  EXPECT_EQ(sellPrice.toString(2), "99.90");
  const Decimal sellAmount = sellPrice.times(lots * lotSize).round(2, RoundingMode::HALF_UP);
  EXPECT_EQ(sellAmount.toString(2), "6993.00");
  // Стоимость позиции по close (п. 6), HALF_UP до копейки: 100.0149 * 70 = 7001.043.
  EXPECT_EQ(D("100.0149").times(70).round(2, RoundingMode::HALF_UP).toString(2), "7001.04");
  // Количество из DSL вниз до целого лота (п. 7): 73 бумаги при лоте 10 -> 7 лотов.
  EXPECT_EQ(D("73").div(Decimal::fromUnits(10), 0, RoundingMode::FLOOR).toString(), "7");
}

TEST(DecimalReference, ExecutionPriceUsesSameDirectedModeInBothSteps) {
  // Правило 1 (не в пользу трейдера): режим CEILING/FLOOR нужен и в умножении, и при
  // округлении до шага. HALF_UP в первом шаге уронил бы значение на границу шага.
  const Decimal step = D("0.01");
  const Decimal buyFactor = D("0.990099010"), sellFactor = D("1.009900990");
  const Decimal open = D("1.01");
  // 1.01 * 0.990099010 = 1.00000000010: покупка должна дать 1.01, а не 1.00.
  EXPECT_EQ(open.mul(buyFactor, RoundingMode::HALF_UP).roundToStep(step, RoundingMode::CEILING).toString(2), "1.00");
  EXPECT_EQ(open.mul(buyFactor, RoundingMode::CEILING).roundToStep(step, RoundingMode::CEILING).toString(2), "1.01");
  // 1.01 * 1.009900990 = 1.01999999990: продажа должна дать 1.01, а не 1.02.
  EXPECT_EQ(open.mul(sellFactor, RoundingMode::HALF_UP).roundToStep(step, RoundingMode::FLOOR).toString(2), "1.02");
  EXPECT_EQ(open.mul(sellFactor, RoundingMode::FLOOR).roundToStep(step, RoundingMode::FLOOR).toString(2), "1.01");
}

// ================================ Property-тест ========================================
namespace {

// Случайное значение в единицах хранения: случайная длина (1..maxBits бит) и знак.
// Только сырой выход mt19937_64 (распределения стандартной библиотеки не переносимы).
Int128 randomRaw(std::mt19937_64& rng, int maxBits) {
  const int bits = 1 + static_cast<int>(rng() % static_cast<std::uint64_t>(maxBits));
  core::UInt128 v = (static_cast<core::UInt128>(rng()) << 64) | rng();
  if (bits < 128) v &= (static_cast<core::UInt128>(1) << bits) - 1;
  Int128 r = static_cast<Int128>(v);
  return (rng() & 1U) ? -r : r;
}

}  // namespace

TEST(DecimalProperty, StringRoundTripAndMulDivInverse) {
  std::mt19937_64 rng(20261103);
  for (int i = 0; i < 10000; ++i) {
    const Decimal a = Decimal::fromRaw(randomRaw(rng, 126));
    // Строка туда-обратно, в том числе с фиксированным числом знаков.
    ASSERT_EQ(Decimal::parse(a.toString()), a) << a.toString();
    ASSERT_EQ(Decimal::parse(a.toString(9)), a);
    ASSERT_EQ(Decimal::parse(a.toString(2 + i % 8)), a);

    // (a*b)/b ~ a: |b| >= 1, поэтому после двух округлений HALF_UP расхождение <= 1 единицы хранения.
    const Decimal x = Decimal::fromRaw(randomRaw(rng, 90));
    Int128 braw = randomRaw(rng, 60);
    if (braw < 0) braw = -braw;
    braw += Decimal::kUnit;  // >= 1.0
    for (const Decimal b : {Decimal::fromRaw(braw), Decimal::fromRaw(-braw)}) {
      const Decimal back = x.mul(b, RoundingMode::HALF_UP).div(b, RoundingMode::HALF_UP);
      const Int128 diff = back.raw() - x.raw();
      ASSERT_LE(diff < 0 ? -diff : diff, 1) << x.toString() << " * " << b.toString();
      // Одним шагом mulDiv точен (a*b/b = a без остатка).
      ASSERT_EQ(x.mulDiv(b, b, 9, RoundingMode::UNNECESSARY), x);
    }
  }
}

TEST(DecimalProperty, MatchesNaiveReferenceOnSmallOperands) {
  // |raw| < 2^62: произведение помещается в Int128, эталон - обычное floor-деление.
  std::mt19937_64 rng(777);
  for (int i = 0; i < 10000; ++i) {
    const Int128 a = randomRaw(rng, 62), b = randomRaw(rng, 62);
    Int128 c = randomRaw(rng, 40);
    if (c == 0) c = 1;
    const int scale = static_cast<int>(rng() % 10);
    const Int128 quantum = pow10(9 - scale);
    const RoundingMode mode = kAllModes[rng() % 7];
    const Decimal da = Decimal::fromRaw(a), db = Decimal::fromRaw(b), dc = Decimal::fromRaw(c);

    // mul: a*b / 1e9, квант 10^(9-scale)
    ASSERT_EQ(da.mul(db, scale, mode).raw(), refRound(a * b, Decimal::kUnit * quantum, mode) * quantum);
    // div: a*1e9 / c (знаменатель - положительный)
    Int128 n = a * Decimal::kUnit, d = c * quantum;
    if (d < 0) { n = -n; d = -d; }
    ASSERT_EQ(da.div(dc, scale, mode).raw(), refRound(n, d, mode) * quantum);
    // mulDiv: a*b / c
    n = a * b;
    d = c * quantum;
    if (d < 0) { n = -n; d = -d; }
    ASSERT_EQ(da.mulDiv(db, dc, scale, mode).raw(), refRound(n, d, mode) * quantum);
    // round и roundToStep
    ASSERT_EQ(da.round(scale, mode).raw(), refRound(a, quantum, mode) * quantum);
    const Int128 stepRaw = (c < 0 ? -c : c);
    ASSERT_EQ(da.roundToStep(Decimal::fromRaw(stepRaw), mode).raw(), refRound(a, stepRaw, mode) * stepRaw);
  }
}

// ================================ Детерминизм (INV-4) ===================================
namespace {

std::vector<unsigned char> runSequence() {
  std::mt19937_64 rng(42);
  std::vector<unsigned char> bytes;
  Decimal acc = Decimal::fromUnits(1000000);
  for (int i = 0; i < 2000; ++i) {
    const Decimal price = Decimal::fromRaw(1 + static_cast<Int128>(rng() % 100000000000ULL));
    const Decimal rate = Decimal::fromRaw(static_cast<Int128>(rng() % 3000000ULL));
    const RoundingMode mode = kAllModes[rng() % 7];
    acc = (acc + price.mul(rate, 2, mode)).mulDiv(D("1.0001"), D("1.0001"), 4, mode);
    acc = acc.roundToStep(D("0.0005"), mode);
    const Decimal::Raw r = acc.raw();
    unsigned char buf[sizeof r];
    std::memcpy(buf, &r, sizeof r);
    bytes.insert(bytes.end(), buf, buf + sizeof r);
  }
  return bytes;
}

}  // namespace

TEST(DecimalDeterminism, SameOperationsGiveIdenticalBytes) {
  const auto first = runSequence();
  const auto second = runSequence();
  ASSERT_EQ(first.size(), second.size());
  EXPECT_EQ(0, std::memcmp(first.data(), second.data(), first.size()));
}
