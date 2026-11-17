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

Decimal d(const char *Text) { return Decimal::parse(Text); }

constexpr RoundingMode kAllModes[] = {
    RoundingMode::UP,      RoundingMode::DOWN,   RoundingMode::CEILING,
    RoundingMode::FLOOR,   RoundingMode::HalfUp, RoundingMode::HalfDown,
    RoundingMode::HalfEven};

// ---- INV-6: из double Decimal не строится
// ---------------------------------------------
static_assert(!std::is_constructible_v<Decimal, double>);
static_assert(!std::is_constructible_v<Decimal, float>);
static_assert(!std::is_constructible_v<Decimal, long double>);
static_assert(!std::is_convertible_v<double, Decimal>);
template <class T>
concept TimesCallable = requires(Decimal D, T V) { D.times(V); };
template <class T>
concept FromUnitsCallable = requires(T V) { Decimal::fromUnits(V); };
static_assert(!TimesCallable<double> && !TimesCallable<float>);
static_assert(!FromUnitsCallable<double> && !FromUnitsCallable<long double>);
static_assert(TimesCallable<std::int64_t> && FromUnitsCallable<std::int64_t>);

// ---- Независимая эталонная реализация округления (только для малых операндов)
// --------- n / d, d > 0, на Int128 через floor-деление; режимы по определению
// java.math.RoundingMode.
Int128 floorDiv(Int128 N, Int128 D) {
  Int128 Q = N / D;
  if ((N % D != 0) && (N < 0))
    --Q;
  return Q;
}

Int128 refRound(Int128 N, Int128 D, RoundingMode Mode) {
  const Int128 Fl = floorDiv(N, D);
  const Int128 Rem = N - Fl * D; // 0 <= rem < d
  if (Rem == 0)
    return Fl;
  const Int128 Ce = Fl + 1;
  const Int128 Up = N > 0 ? Ce : Fl;   // от нуля
  const Int128 Down = N > 0 ? Fl : Ce; // к нулю
  switch (Mode) {
  case RoundingMode::UP:
    return Up;
  case RoundingMode::DOWN:
    return Down;
  case RoundingMode::CEILING:
    return Ce;
  case RoundingMode::FLOOR:
    return Fl;
  default:
    break;
  }
  const Int128 Twice = Rem * 2;
  if (Twice > D)
    return Ce;
  if (Twice < D)
    return Fl;
  switch (Mode) {
  case RoundingMode::HalfUp:
    return Up;
  case RoundingMode::HalfDown:
    return Down;
  default:
    return (Fl % 2 == 0) ? Fl : Ce; // HALF_EVEN
  }
}

Int128 pow10(int K) {
  Int128 P = 1;
  while (K-- > 0)
    P *= 10;
  return P;
}

} // namespace

// ================================ Разбор и печать
// ======================================
TEST(DecimalString, ParseAndCanonicalForm) {
  EXPECT_EQ(d("123.45").toString(), "123.45");
  EXPECT_EQ(d("-0.000000001").toString(), "-0.000000001");
  EXPECT_EQ(d("0").toString(), "0");
  EXPECT_EQ(d("-0").toString(), "0");
  EXPECT_EQ(d("5.0").toString(), "5");
  EXPECT_EQ(d("007.500").toString(), "7.5");
  EXPECT_EQ(d("0.000000001").raw(), 1);
  EXPECT_EQ(d("1").raw(), 1000000000);
  EXPECT_EQ(d("123.45").toString(2), "123.45");
  EXPECT_EQ(d("5").toString(2), "5.00");
  EXPECT_EQ(d("-0.5").toString(9), "-0.500000000");
  EXPECT_EQ(Decimal().toString(2), "0.00");
}

TEST(DecimalString, RejectsMalformedInput) {
  for (const char *Bad :
       {"", "-", ".", "1.", ".5", "+1", "1e5", " 1", "1 ", "1.2.3", "--1",
        "0.0000000001", "abc", "1,5", "1.-5", "- 1"}) {
    EXPECT_THROW(Decimal::parse(Bad), std::invalid_argument) << Bad;
    EXPECT_FALSE(Decimal::tryParse(Bad).has_value()) << Bad;
  }
  EXPECT_TRUE(Decimal::tryParse("1.5").has_value());
}

TEST(DecimalString, RangeBoundaryIsInt128Max) {
  const char *KMax =
      "170141183460469231731687303715.884105727"; // 2^127 - 1 единиц 10^-9
  const Decimal Max = d(KMax);
  EXPECT_EQ(Max.raw(), Decimal::kMaxRaw);
  EXPECT_EQ(Max.toString(), KMax);
  EXPECT_EQ((-Max).toString(), std::string("-") + KMax);
  EXPECT_THROW(Decimal::parse("170141183460469231731687303715.884105728"),
               DecimalOverflow);
  EXPECT_THROW(Decimal::parse("-170141183460469231731687303715.884105728"),
               DecimalOverflow);
  EXPECT_THROW(Decimal::parse("999999999999999999999999999999999999999"),
               DecimalOverflow);
  EXPECT_FALSE(Decimal::tryParse("170141183460469231731687303716").has_value());
}

// ================================ Арифметика
// ===========================================
TEST(DecimalArithmetic, ExactAddSubtractCompare) {
  EXPECT_EQ((d("0.1") + d("0.2")).toString(),
            "0.3"); // на double было бы 0.30000000000000004
  EXPECT_EQ((d("0.3") - d("0.1")).toString(), "0.2");
  EXPECT_EQ((d("1") - d("1.000000001")).toString(), "-0.000000001");
  EXPECT_EQ((-d("2.5")).toString(), "-2.5");
  EXPECT_EQ(d("-2.5").abs().toString(), "2.5");
  Decimal Acc = d("10");
  Acc += d("0.01");
  Acc -= d("0.02");
  EXPECT_EQ(Acc.toString(), "9.99");
  EXPECT_LT(d("-1"), d("0"));
  EXPECT_GT(d("1.000000001"), d("1"));
  EXPECT_EQ(d("1.50"), d("1.5"));
  EXPECT_NE(d("1.5"), d("1.6"));
  EXPECT_TRUE(Decimal().isZero());
  EXPECT_EQ(d("-3").sign(), -1);
  EXPECT_EQ(d("3").sign(), 1);
  EXPECT_EQ(Decimal::zero().sign(), 0);
}

TEST(DecimalArithmetic, TimesInteger) {
  EXPECT_EQ(d("100.16").times(70).toString(), "7011.2");
  EXPECT_EQ(d("0.000000001").times(-3).toString(), "-0.000000003");
  EXPECT_EQ(Decimal::fromUnits(1).times(0), Decimal());
}

TEST(DecimalArithmetic, OverflowThrows) {
  const Decimal Max = Decimal::fromRaw(Decimal::kMaxRaw);
  EXPECT_THROW(Max + d("0.000000001"), DecimalOverflow);
  EXPECT_THROW((-Max) - d("0.000000001"), DecimalOverflow);
  EXPECT_THROW(Max.times(2), DecimalOverflow);
  EXPECT_THROW(Max.mul(d("2"), RoundingMode::DOWN), DecimalOverflow);
  EXPECT_THROW(Max.div(d("0.5"), RoundingMode::DOWN), DecimalOverflow);
  EXPECT_THROW(Decimal::fromRaw(-Decimal::kMaxRaw - 1), DecimalOverflow);
  const Decimal Big =
      Decimal::fromUnits(std::numeric_limits<std::int64_t>::max());
  EXPECT_THROW(Big.mul(Big, RoundingMode::HalfUp), DecimalOverflow);
  EXPECT_EQ((Max - Max), Decimal());
  EXPECT_EQ(Max.mul(d("1"), RoundingMode::DOWN), Max);
}

TEST(DecimalArithmetic, DivisionByZeroThrows) {
  EXPECT_THROW(d("1").div(Decimal(), RoundingMode::HalfUp), std::domain_error);
  EXPECT_THROW(Decimal().div(Decimal(), RoundingMode::HalfUp),
               std::domain_error);
  EXPECT_THROW(d("1").mulDiv(d("2"), Decimal(), 2, RoundingMode::HalfUp),
               std::domain_error);
}

// ================================ Округление
// ===========================================
namespace {
struct RoundCase {
  const char *In;
  const char *Expected[7]; // по порядку kAllModes
};
} // namespace

TEST(DecimalRounding, EveryModeToWholeUnits) {
  // Таблица из документации java.math.RoundingMode.
  const RoundCase Cases[] = {
      //           UP    DOWN   CEIL   FLOOR  H_UP   H_DOWN H_EVEN
      {"5.5", {"6", "5", "6", "5", "6", "5", "6"}},
      {"2.5", {"3", "2", "3", "2", "3", "2", "2"}},
      {"1.6", {"2", "1", "2", "1", "2", "2", "2"}},
      {"1.1", {"2", "1", "2", "1", "1", "1", "1"}},
      {"1.0", {"1", "1", "1", "1", "1", "1", "1"}},
      {"0", {"0", "0", "0", "0", "0", "0", "0"}},
      {"-1.0", {"-1", "-1", "-1", "-1", "-1", "-1", "-1"}},
      {"-1.1", {"-2", "-1", "-1", "-2", "-1", "-1", "-1"}},
      {"-1.6", {"-2", "-1", "-1", "-2", "-2", "-2", "-2"}},
      {"-2.5", {"-3", "-2", "-2", "-3", "-3", "-2", "-2"}},
      {"-5.5", {"-6", "-5", "-5", "-6", "-6", "-5", "-6"}},
      {"0.5", {"1", "0", "1", "0", "1", "0", "0"}},
      {"-0.5", {"-1", "0", "0", "-1", "-1", "0", "0"}},
      {"0.4", {"1", "0", "1", "0", "0", "0", "0"}},
      {"-0.4", {"-1", "0", "0", "-1", "0", "0", "0"}},
  };
  for (const auto &C : Cases) {
    for (std::size_t I = 0; I < 7; ++I) {
      EXPECT_EQ(d(C.In).round(0, kAllModes[I]).toString(), C.Expected[I])
          << C.In << " mode#" << I;
    }
  }
}

TEST(DecimalRounding, ToKopecksAndOtherScales) {
  EXPECT_EQ(d("3.5056").round(2, RoundingMode::CEILING).toString(2), "3.51");
  EXPECT_EQ(d("3.5001").round(2, RoundingMode::CEILING).toString(2), "3.51");
  EXPECT_EQ(d("3.50").round(2, RoundingMode::CEILING).toString(2),
            "3.50"); // ровно - не округляем
  EXPECT_EQ(d("0.005").round(2, RoundingMode::HalfUp).toString(2), "0.01");
  EXPECT_EQ(d("-0.005").round(2, RoundingMode::HalfUp).toString(2),
            "-0.01"); // от нуля
  EXPECT_EQ(d("0.004999999").round(2, RoundingMode::HalfUp).toString(2),
            "0.00");
  EXPECT_EQ(d("0.015").round(2, RoundingMode::HalfEven).toString(2), "0.02");
  EXPECT_EQ(d("0.025").round(2, RoundingMode::HalfEven).toString(2), "0.02");
  EXPECT_EQ(d("-0.025").round(2, RoundingMode::HalfDown).toString(2), "-0.02");
  EXPECT_EQ(d("-7.777777777").round(6, RoundingMode::FLOOR).toString(),
            "-7.777778");
  // scale 9 - без изменений
  EXPECT_EQ(d("1.123456789").round(9, RoundingMode::UNNECESSARY).toString(),
            "1.123456789");
}

TEST(DecimalRounding, UnnecessaryThrowsOnlyWhenRoundingNeeded) {
  EXPECT_EQ(d("1.20").round(1, RoundingMode::UNNECESSARY).toString(), "1.2");
  EXPECT_THROW(d("1.25").round(1, RoundingMode::UNNECESSARY),
               std::domain_error);
  EXPECT_THROW(d("1").div(d("3"), RoundingMode::UNNECESSARY),
               std::domain_error);
  EXPECT_EQ(d("1").div(d("4"), RoundingMode::UNNECESSARY).toString(), "0.25");
  EXPECT_THROW(d("1.01").roundToStep(d("0.05"), RoundingMode::UNNECESSARY),
               std::domain_error);
  EXPECT_EQ(
      d("1.05").roundToStep(d("0.05"), RoundingMode::UNNECESSARY).toString(),
      "1.05");
}

TEST(DecimalRounding, InvalidScaleThrows) {
  EXPECT_THROW(d("1").round(-1, RoundingMode::DOWN), std::invalid_argument);
  EXPECT_THROW(d("1").round(10, RoundingMode::DOWN), std::invalid_argument);
  EXPECT_THROW(d("1").mul(d("1"), 10, RoundingMode::DOWN),
               std::invalid_argument);
  EXPECT_THROW(d("1").toString(10), std::invalid_argument);
}

TEST(DecimalRounding, ToPriceStep) {
  // decisions.md, п. 1-2: покупка - CEILING, продажа - FLOOR, DSL и слепой
  // масштаб - HALF_UP.
  const Decimal Step = d("0.01");
  EXPECT_EQ(
      d("100.1550375").roundToStep(Step, RoundingMode::CEILING).toString(2),
      "100.16");
  EXPECT_EQ(d("100.1550375").roundToStep(Step, RoundingMode::FLOOR).toString(2),
            "100.15");
  EXPECT_EQ(
      d("100.1550375").roundToStep(Step, RoundingMode::HalfUp).toString(2),
      "100.16");
  EXPECT_EQ(d("100.15").roundToStep(Step, RoundingMode::CEILING).toString(2),
            "100.15");
  EXPECT_EQ(d("99.9049625").roundToStep(Step, RoundingMode::FLOOR).toString(2),
            "99.90");
  // Шаг 0.05, ровно половина шага.
  const Decimal S5 = d("0.05");
  EXPECT_EQ(d("100.125").roundToStep(S5, RoundingMode::HalfUp).toString(2),
            "100.15");
  EXPECT_EQ(d("100.125").roundToStep(S5, RoundingMode::HalfDown).toString(2),
            "100.10");
  EXPECT_EQ(d("100.125").roundToStep(S5, RoundingMode::HalfEven).toString(2),
            "100.10"); // 2002 - чётное
  EXPECT_EQ(d("100.175").roundToStep(S5, RoundingMode::HalfEven).toString(2),
            "100.20"); // 2003 -> 2004
  EXPECT_EQ(d("-100.125").roundToStep(S5, RoundingMode::HalfUp).toString(2),
            "-100.15");
  EXPECT_EQ(d("-100.125").roundToStep(S5, RoundingMode::FLOOR).toString(2),
            "-100.15");
  EXPECT_EQ(d("-100.125").roundToStep(S5, RoundingMode::CEILING).toString(2),
            "-100.10");
  EXPECT_EQ(d("-100.121").roundToStep(S5, RoundingMode::UP).toString(2),
            "-100.15");
  EXPECT_EQ(d("-100.121").roundToStep(S5, RoundingMode::DOWN).toString(2),
            "-100.10");
  // Шаг, не являющийся степенью десяти и крупнее единицы.
  EXPECT_EQ(d("17").roundToStep(d("5"), RoundingMode::HalfUp).toString(), "15");
  EXPECT_EQ(d("17.5").roundToStep(d("5"), RoundingMode::HalfUp).toString(),
            "20");
  EXPECT_EQ(d("0.000000007")
                .roundToStep(d("0.000000005"), RoundingMode::CEILING)
                .toString(),
            "0.00000001");
  EXPECT_EQ(d("12.345678912")
                .roundToStep(d("0.000000001"), RoundingMode::FLOOR)
                .toString(),
            "12.345678912");
  EXPECT_THROW(d("1").roundToStep(Decimal(), RoundingMode::DOWN),
               std::invalid_argument);
  EXPECT_THROW(d("1").roundToStep(d("-0.01"), RoundingMode::DOWN),
               std::invalid_argument);
}

TEST(DecimalRounding, MultiplyAndDivideEveryMode) {
  // 1.000000001^2 = 1.000000002000000001 -> девять знаков.
  const Decimal X = d("1.000000001");
  EXPECT_EQ(X.mul(X, RoundingMode::DOWN).toString(), "1.000000002");
  EXPECT_EQ(X.mul(X, RoundingMode::UP).toString(), "1.000000003");
  EXPECT_EQ(X.mul(X, RoundingMode::CEILING).toString(), "1.000000003");
  EXPECT_EQ(X.mul(X, RoundingMode::FLOOR).toString(), "1.000000002");
  EXPECT_EQ(X.mul(X, RoundingMode::HalfUp).toString(), "1.000000002");
  EXPECT_EQ((-X).mul(X, RoundingMode::CEILING).toString(), "-1.000000002");
  EXPECT_EQ((-X).mul(X, RoundingMode::FLOOR).toString(), "-1.000000003");
  EXPECT_EQ((-X).mul(-X, RoundingMode::UP).toString(), "1.000000003");
  // Ровно половина единицы хранения: 0.000000001 * 0.5, 0.000000003 * 0.5.
  const Decimal Half = d("0.5");
  const struct {
    const char *A;
    const char *Expected[7];
  } Halves[] = {
      {"0.000000001",
       {"0.000000001", "0", "0.000000001", "0", "0.000000001", "0", "0"}},
      {"0.000000003",
       {"0.000000002", "0.000000001", "0.000000002", "0.000000001",
        "0.000000002", "0.000000001", "0.000000002"}},
      {"-0.000000001",
       {"-0.000000001", "0", "0", "-0.000000001", "-0.000000001", "0", "0"}},
  };
  for (const auto &H : Halves) {
    for (std::size_t I = 0; I < 7; ++I) {
      EXPECT_EQ(d(H.A).mul(Half, kAllModes[I]).toString(), H.Expected[I])
          << H.A << " mode#" << I;
    }
  }
  // Деление: 1/3, 2/3, -2/3.
  const Decimal One = d("1"), Two = d("2"), Three = d("3");
  EXPECT_EQ(One.div(Three, RoundingMode::DOWN).toString(), "0.333333333");
  EXPECT_EQ(One.div(Three, RoundingMode::UP).toString(), "0.333333334");
  EXPECT_EQ(One.div(Three, RoundingMode::HalfUp).toString(), "0.333333333");
  EXPECT_EQ(Two.div(Three, RoundingMode::HalfUp).toString(), "0.666666667");
  EXPECT_EQ(Two.div(Three, RoundingMode::HalfEven).toString(), "0.666666667");
  EXPECT_EQ((-Two).div(Three, RoundingMode::FLOOR).toString(), "-0.666666667");
  EXPECT_EQ((-Two).div(Three, RoundingMode::CEILING).toString(),
            "-0.666666666");
  EXPECT_EQ(Two.div(-Three, RoundingMode::HalfDown).toString(), "-0.666666667");
  EXPECT_EQ(d("0.000000001").div(Two, RoundingMode::HalfUp).toString(),
            "0.000000001");
  EXPECT_EQ(d("0.000000001").div(Two, RoundingMode::HalfDown).toString(), "0");
  // Прямое округление до знака без двойного округления: 0.0149 * 1 -> копейки.
  EXPECT_EQ(d("0.0149").mul(One, 2, RoundingMode::HalfUp).toString(2), "0.01");
  EXPECT_EQ(d("0.0149").mul(One, 2, RoundingMode::CEILING).toString(2), "0.02");
  // Умножение на ноль и ноль в числителе.
  EXPECT_EQ(Decimal().div(Three, RoundingMode::UP), Decimal());
  EXPECT_EQ(Three.mul(Decimal(), RoundingMode::UP), Decimal());
}

TEST(DecimalRounding, MulDivBlindScaleSingleStep) {
  // decisions.md, п. 8: показанная цена = цена * 100 / P0, 6 знаков HALF_UP.
  const Decimal Price = d("253.456789"), P0 = d("250.12");
  EXPECT_EQ(Price.mulDiv(d("100"), P0, 6, RoundingMode::HalfUp).toString(),
            "101.334075");
  EXPECT_EQ(Price.mulDiv(d("100"), P0, 9, RoundingMode::HalfUp).toString(),
            "101.334075244");
  // Обратно: цена = показанная * P0 / 100, затем шаг цены HALF_UP.
  const Decimal Shown = d("101.334075");
  EXPECT_EQ(Shown.mulDiv(P0, d("100"), 9, RoundingMode::HalfUp)
                .roundToStep(d("0.01"), RoundingMode::HalfUp)
                .toString(2),
            "253.46");
  EXPECT_EQ(d("100").mulDiv(d("100"), d("100"), 6, RoundingMode::HalfUp),
            d("100"));
}

TEST(DecimalRounding, WideIntermediateMatchesPythonReference) {
  // 256-битный путь: a*b не помещается в 128 бит. Эталон посчитан Python
  // (Fraction).
  const Decimal A = d("123456789012345678901.123456789");
  const Decimal B = d("98765432109876543210.987654321");
  const Decimal C = d("11111111111111111111.5");
  EXPECT_EQ(A.mulDiv(B, C, 9, RoundingMode::DOWN).toString(),
            "1097393680233196156996.268861563");
  EXPECT_EQ(A.mulDiv(B, C, 9, RoundingMode::UP).toString(),
            "1097393680233196156996.268861564");
  EXPECT_EQ(A.mulDiv(B, C, 9, RoundingMode::HalfUp).toString(),
            "1097393680233196156996.268861563");
  EXPECT_EQ(A.mulDiv(B, C, 2, RoundingMode::DOWN).toString(),
            "1097393680233196156996.26");
  EXPECT_EQ(A.mulDiv(B, C, 2, RoundingMode::HalfUp).toString(),
            "1097393680233196156996.27");
  EXPECT_EQ((-A).mulDiv(B, C, 6, RoundingMode::DOWN).toString(),
            "-1097393680233196156996.268861");
  EXPECT_EQ((-A).mulDiv(B, C, 6, RoundingMode::UP).toString(),
            "-1097393680233196156996.268862");
  EXPECT_EQ((-A).mulDiv(B, C, 6, RoundingMode::HalfUp).toString(),
            "-1097393680233196156996.268862");
  // Результат не помещается -> переполнение, а не молчаливое усечение.
  EXPECT_THROW(A.mulDiv(B, d("0.000000001"), 9, RoundingMode::DOWN),
               DecimalOverflow);
}

// ================================ Эталонная сделка
// =====================================
TEST(DecimalReference, TradeCycleByRules) {
  // Бумага: шаг цены 0.01, лот 10. Open = 100.03; спред/проскальзывание дают
  // множитель.
  const Decimal Open = d("100.03"), Step = d("0.01");
  const Decimal CommissionRate = d("0.0005"), ExchangeRate = d("0.0001");
  const std::int64_t Lots = 7, LotSize = 10;

  // Покупка: 100.03 * 1.00125 = 100.1550375 -> шаг CEILING = 100.16.
  const Decimal BuyPrice = Open.mul(d("1.00125"), RoundingMode::CEILING)
                               .roundToStep(Step, RoundingMode::CEILING);
  EXPECT_EQ(BuyPrice.toString(2), "100.16");
  // Сумма = 100.16 * 7 * 10 = 7011.20 (HALF_UP до копейки, здесь точно).
  const Decimal Amount =
      BuyPrice.times(Lots * LotSize).round(2, RoundingMode::HalfUp);
  EXPECT_EQ(Amount.toString(2), "7011.20");
  // Комиссия 3.5056 -> 3.51, сбор 0.70112 -> 0.71 (каждая отдельно, CEILING).
  const Decimal Commission =
      Amount.mul(CommissionRate, 2, RoundingMode::CEILING);
  const Decimal Fee = Amount.mul(ExchangeRate, 2, RoundingMode::CEILING);
  EXPECT_EQ(Commission.toString(2), "3.51");
  EXPECT_EQ(Fee.toString(2), "0.71");
  // Денежный остаток: 100000.00 - 7011.20 - 3.51 - 0.71 = 92984.58, кратно
  // копейке.
  Decimal Cash = d("100000");
  Cash -= Amount;
  Cash -= Commission;
  Cash -= Fee;
  EXPECT_EQ(Cash.toString(2), "92984.58");
  EXPECT_EQ(Cash, Cash.round(2, RoundingMode::UNNECESSARY));

  // Продажа: 100.03 * 0.99875 = 99.9049625 -> шаг FLOOR = 99.90 (не в пользу
  // трейдера).
  const Decimal SellPrice = Open.mul(d("0.99875"), RoundingMode::FLOOR)
                                .roundToStep(Step, RoundingMode::FLOOR);
  EXPECT_EQ(SellPrice.toString(2), "99.90");
  const Decimal SellAmount =
      SellPrice.times(Lots * LotSize).round(2, RoundingMode::HalfUp);
  EXPECT_EQ(SellAmount.toString(2), "6993.00");
  // Стоимость позиции по close (п. 6), HALF_UP до копейки: 100.0149 * 70 =
  // 7001.043.
  EXPECT_EQ(d("100.0149").times(70).round(2, RoundingMode::HalfUp).toString(2),
            "7001.04");
  // Количество из DSL вниз до целого лота (п. 7): 73 бумаги при лоте 10 -> 7
  // лотов.
  EXPECT_EQ(
      d("73").div(Decimal::fromUnits(10), 0, RoundingMode::FLOOR).toString(),
      "7");
}

TEST(DecimalReference, ExecutionPriceUsesSameDirectedModeInBothSteps) {
  // Правило 1 (не в пользу трейдера): режим CEILING/FLOOR нужен и в умножении,
  // и при округлении до шага. HALF_UP в первом шаге уронил бы значение на
  // границу шага.
  const Decimal Step = d("0.01");
  const Decimal BuyFactor = d("0.990099010"), SellFactor = d("1.009900990");
  const Decimal Open = d("1.01");
  // 1.01 * 0.990099010 = 1.00000000010: покупка должна дать 1.01, а не 1.00.
  EXPECT_EQ(Open.mul(BuyFactor, RoundingMode::HalfUp)
                .roundToStep(Step, RoundingMode::CEILING)
                .toString(2),
            "1.00");
  EXPECT_EQ(Open.mul(BuyFactor, RoundingMode::CEILING)
                .roundToStep(Step, RoundingMode::CEILING)
                .toString(2),
            "1.01");
  // 1.01 * 1.009900990 = 1.01999999990: продажа должна дать 1.01, а не 1.02.
  EXPECT_EQ(Open.mul(SellFactor, RoundingMode::HalfUp)
                .roundToStep(Step, RoundingMode::FLOOR)
                .toString(2),
            "1.02");
  EXPECT_EQ(Open.mul(SellFactor, RoundingMode::FLOOR)
                .roundToStep(Step, RoundingMode::FLOOR)
                .toString(2),
            "1.01");
}

// ================================ Property-тест
// ========================================
namespace {

// Случайное значение в единицах хранения: случайная длина (1..maxBits бит) и
// знак. Только сырой выход mt19937_64 (распределения стандартной библиотеки не
// переносимы).
Int128 randomRaw(std::mt19937_64 &Rng, int MaxBits) {
  const int Bits =
      1 + static_cast<int>(Rng() % static_cast<std::uint64_t>(MaxBits));
  core::UInt128 V = (static_cast<core::UInt128>(Rng()) << 64) | Rng();
  if (Bits < 128)
    V &= (static_cast<core::UInt128>(1) << Bits) - 1;
  Int128 R = static_cast<Int128>(V);
  return (Rng() & 1U) ? -R : R;
}

} // namespace

TEST(DecimalProperty, StringRoundTripAndMulDivInverse) {
  std::mt19937_64 Rng(20261103);
  for (int I = 0; I < 10000; ++I) {
    const Decimal A = Decimal::fromRaw(randomRaw(Rng, 126));
    // Строка туда-обратно, в том числе с фиксированным числом знаков.
    ASSERT_EQ(Decimal::parse(A.toString()), A) << A.toString();
    ASSERT_EQ(Decimal::parse(A.toString(9)), A);
    ASSERT_EQ(Decimal::parse(A.toString(2 + I % 8)), A);

    // (a*b)/b ~ a: |b| >= 1, поэтому после двух округлений HALF_UP расхождение
    // <= 1 единицы хранения.
    const Decimal X = Decimal::fromRaw(randomRaw(Rng, 90));
    Int128 Braw = randomRaw(Rng, 60);
    if (Braw < 0)
      Braw = -Braw;
    Braw += Decimal::kUnit; // >= 1.0
    for (const Decimal B : {Decimal::fromRaw(Braw), Decimal::fromRaw(-Braw)}) {
      const Decimal Back =
          X.mul(B, RoundingMode::HalfUp).div(B, RoundingMode::HalfUp);
      const Int128 Diff = Back.raw() - X.raw();
      ASSERT_LE(Diff < 0 ? -Diff : Diff, 1)
          << X.toString() << " * " << B.toString();
      // Одним шагом mulDiv точен (a*b/b = a без остатка).
      ASSERT_EQ(X.mulDiv(B, B, 9, RoundingMode::UNNECESSARY), X);
    }
  }
}

TEST(DecimalProperty, MatchesNaiveReferenceOnSmallOperands) {
  // |raw| < 2^62: произведение помещается в Int128, эталон - обычное
  // floor-деление.
  std::mt19937_64 Rng(777);
  for (int I = 0; I < 10000; ++I) {
    const Int128 A = randomRaw(Rng, 62), B = randomRaw(Rng, 62);
    Int128 C = randomRaw(Rng, 40);
    if (C == 0)
      C = 1;
    const int Scale = static_cast<int>(Rng() % 10);
    const Int128 Quantum = pow10(9 - Scale);
    const RoundingMode Mode = kAllModes[Rng() % 7];
    const Decimal Da = Decimal::fromRaw(A), Db = Decimal::fromRaw(B),
                  Dc = Decimal::fromRaw(C);

    // mul: a*b / 1e9, квант 10^(9-scale)
    ASSERT_EQ(Da.mul(Db, Scale, Mode).raw(),
              refRound(A * B, Decimal::kUnit * Quantum, Mode) * Quantum);
    // div: a*1e9 / c (знаменатель - положительный)
    Int128 N = A * Decimal::kUnit, D = C * Quantum;
    if (D < 0) {
      N = -N;
      D = -D;
    }
    ASSERT_EQ(Da.div(Dc, Scale, Mode).raw(), refRound(N, D, Mode) * Quantum);
    // mulDiv: a*b / c
    N = A * B;
    D = C * Quantum;
    if (D < 0) {
      N = -N;
      D = -D;
    }
    ASSERT_EQ(Da.mulDiv(Db, Dc, Scale, Mode).raw(),
              refRound(N, D, Mode) * Quantum);
    // round и roundToStep
    ASSERT_EQ(Da.round(Scale, Mode).raw(),
              refRound(A, Quantum, Mode) * Quantum);
    const Int128 StepRaw = (C < 0 ? -C : C);
    ASSERT_EQ(Da.roundToStep(Decimal::fromRaw(StepRaw), Mode).raw(),
              refRound(A, StepRaw, Mode) * StepRaw);
  }
}

// ================================ Детерминизм (INV-4)
// ===================================
namespace {

std::vector<unsigned char> runSequence() {
  std::mt19937_64 Rng(42);
  std::vector<unsigned char> Bytes;
  Decimal Acc = Decimal::fromUnits(1000000);
  for (int I = 0; I < 2000; ++I) {
    const Decimal Price =
        Decimal::fromRaw(1 + static_cast<Int128>(Rng() % 100000000000ULL));
    const Decimal Rate =
        Decimal::fromRaw(static_cast<Int128>(Rng() % 3000000ULL));
    const RoundingMode Mode = kAllModes[Rng() % 7];
    Acc = (Acc + Price.mul(Rate, 2, Mode))
              .mulDiv(d("1.0001"), d("1.0001"), 4, Mode);
    Acc = Acc.roundToStep(d("0.0005"), Mode);
    const Decimal::Raw R = Acc.raw();
    unsigned char Buf[sizeof R];
    std::memcpy(Buf, &R, sizeof R);
    Bytes.insert(Bytes.end(), Buf, Buf + sizeof R);
  }
  return Bytes;
}

} // namespace

TEST(DecimalDeterminism, SameOperationsGiveIdenticalBytes) {
  const auto First = runSequence();
  const auto Second = runSequence();
  ASSERT_EQ(First.size(), Second.size());
  EXPECT_EQ(0, std::memcmp(First.data(), Second.data(), First.size()));
}
