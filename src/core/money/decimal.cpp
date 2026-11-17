#include "core/money/decimal.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <ostream>

namespace core {
namespace {

constexpr UInt128 kMaxMag = static_cast<UInt128>(Decimal::kMaxRaw);
constexpr UInt128 kUnit = static_cast<UInt128>(Decimal::kUnit);

constexpr UInt128 pow10ForScale(int Scale) {
  // Размер кванта в единицах хранения: 10^(9 - scale).
  UInt128 M = 1;
  for (int I = Scale; I < Decimal::kScale; ++I)
    M *= 10;
  return M;
}

void checkScale(int Scale) {
  if (Scale < 0 || Scale > Decimal::kScale) {
    throw std::invalid_argument("Decimal: scale must be in [0, 9]");
  }
}

[[noreturn]] void throwOverflow() {
  throw DecimalOverflow("Decimal: overflow");
}

UInt128 magnitude(Decimal::Raw R) {
  return R < 0 ? static_cast<UInt128>(-R) : static_cast<UInt128>(R);
}

Decimal::Raw withSign(bool Negative, UInt128 Mag) {
  if (Mag > kMaxMag)
    throwOverflow();
  const auto V = static_cast<Decimal::Raw>(Mag);
  return Negative ? -V : V;
}

// Дробная часть отбрасываемого остатка относительно кванта.
enum class Frac { Zero, Below, Half, Above };

Frac fracFromRemainder(UInt128 Rem, UInt128 Quantum) {
  if (Rem == 0)
    return Frac::Zero;
  const UInt128 Other = Quantum - Rem;
  if (Rem < Other)
    return Frac::Below;
  if (Rem == Other)
    return Frac::Half;
  return Frac::Above;
}

// lower - модуль, усечённый к кратному кванту; lowerQuotientOdd - чётность
// lower / quantum (нужна HALF_EVEN). Возвращает окончательное значение со
// знаком.
Decimal::Raw finishRounding(bool Negative, UInt128 Lower, UInt128 Quantum,
                            bool LowerQuotientOdd, Frac Frac,
                            RoundingMode Mode) {
  bool Increment = false;
  switch (Mode) {
  case RoundingMode::UP:
    Increment = Frac != Frac::Zero;
    break;
  case RoundingMode::DOWN:
    break;
  case RoundingMode::CEILING:
    Increment = !Negative && Frac != Frac::Zero;
    break;
  case RoundingMode::FLOOR:
    Increment = Negative && Frac != Frac::Zero;
    break;
  case RoundingMode::HalfUp:
    Increment = Frac == Frac::Half || Frac == Frac::Above;
    break;
  case RoundingMode::HalfDown:
    Increment = Frac == Frac::Above;
    break;
  case RoundingMode::HalfEven:
    Increment = Frac == Frac::Above || (Frac == Frac::Half && LowerQuotientOdd);
    break;
  case RoundingMode::UNNECESSARY:
    if (Frac != Frac::Zero)
      throw std::domain_error("Decimal: rounding necessary");
    break;
  }
  if (Lower > kMaxMag)
    throwOverflow();
  if (Increment) {
    if (Quantum > kMaxMag - Lower)
      throwOverflow();
    Lower += Quantum;
  }
  return withSign(Negative, Lower);
}

// ---- 256-битная арифметика для умножения и деления без потерь
// ------------------------
struct U256 {
  std::array<std::uint64_t, 4> W{}; // little-endian
};

U256 mul128(UInt128 A, UInt128 B) {
  const std::uint64_t A0 = static_cast<std::uint64_t>(A);
  const std::uint64_t A1 = static_cast<std::uint64_t>(A >> 64);
  const std::uint64_t B0 = static_cast<std::uint64_t>(B);
  const std::uint64_t B1 = static_cast<std::uint64_t>(B >> 64);
  const UInt128 P00 = static_cast<UInt128>(A0) * B0;
  const UInt128 P01 = static_cast<UInt128>(A0) * B1;
  const UInt128 P10 = static_cast<UInt128>(A1) * B0;
  const UInt128 P11 = static_cast<UInt128>(A1) * B1;
  const UInt128 Mid = (P00 >> 64) + static_cast<std::uint64_t>(P01) +
                      static_cast<std::uint64_t>(P10);
  const UInt128 Hi = (Mid >> 64) + (P01 >> 64) + (P10 >> 64) + P11;
  U256 R;
  R.W[0] = static_cast<std::uint64_t>(P00);
  R.W[1] = static_cast<std::uint64_t>(Mid);
  R.W[2] = static_cast<std::uint64_t>(Hi);
  R.W[3] = static_cast<std::uint64_t>(Hi >> 64);
  return R;
}

struct DivResult {
  U256 Quotient;
  UInt128 Remainder = 0;
};

// n / d, d > 0 и d <= kMaxMag (поэтому сдвиг остатка не переполняет 128 бит).
DivResult divide(const U256 &N, UInt128 D) {
  DivResult Res;
  if (N.W[2] == 0 && N.W[3] == 0) {
    const UInt128 Low = (static_cast<UInt128>(N.W[1]) << 64) | N.W[0];
    const UInt128 Q = Low / D;
    Res.Remainder = Low % D;
    Res.Quotient.W[0] = static_cast<std::uint64_t>(Q);
    Res.Quotient.W[1] = static_cast<std::uint64_t>(Q >> 64);
    return Res;
  }
  int Top = 255;
  while (Top >= 0 && ((N.W[Top / 64] >> (Top % 64)) & 1U) == 0)
    --Top;
  UInt128 Rem = 0;
  for (int I = Top; I >= 0; --I) {
    Rem = (Rem << 1) | ((N.W[I / 64] >> (I % 64)) & 1U);
    if (Rem >= D) {
      Rem -= D;
      Res.Quotient.W[I / 64] |= std::uint64_t{1} << (I % 64);
    }
  }
  Res.Remainder = Rem;
  return Res;
}

// a * b / c в единицах хранения, результат округляется до кратного quantum (>=
// 1) одним шагом.
Decimal::Raw mulDivRound(Decimal::Raw A, Decimal::Raw B, Decimal::Raw C,
                         UInt128 Quantum, RoundingMode Mode) {
  if (C == 0)
    throw std::domain_error("Decimal: division by zero");
  if (A == 0 || B == 0)
    return 0;
  const bool Negative = ((A < 0) != (B < 0)) != (C < 0);
  const UInt128 D = magnitude(C);
  const DivResult Dr = divide(mul128(magnitude(A), magnitude(B)), D);
  if (Dr.Quotient.W[2] != 0 || Dr.Quotient.W[3] != 0)
    throwOverflow();
  const UInt128 Q =
      (static_cast<UInt128>(Dr.Quotient.W[1]) << 64) | Dr.Quotient.W[0];

  const UInt128 Qm = Q / Quantum;
  const UInt128 Qr = Q % Quantum;
  Frac Frac;
  if (Qr == 0 && Dr.Remainder == 0) {
    Frac = Frac::Zero;
  } else if (Quantum == 1) {
    // Остаток деления r / d в (0, 1): сравниваем 2r с d (2r < 2^128, так как r
    // < d <= 2^127).
    const UInt128 Twice = Dr.Remainder * 2;
    Frac = Twice < D ? Frac::Below : (Twice == D ? Frac::Half : Frac::Above);
  } else {
    const UInt128 Half =
        Quantum / 2; // квант - степень десяти >= 10, делится на 2
    if (Qr < Half) {
      Frac = Frac::Below;
    } else if (Qr > Half) {
      Frac = Frac::Above;
    } else {
      Frac = Dr.Remainder == 0 ? Frac::Half : Frac::Above;
    }
  }
  return finishRounding(Negative, Qm * Quantum, Quantum, (Qm & 1U) != 0, Frac,
                        Mode);
}

Decimal::Raw roundToMultiple(Decimal::Raw Value, UInt128 Quantum,
                             RoundingMode Mode) {
  const UInt128 Mag = magnitude(Value);
  const UInt128 Qm = Mag / Quantum;
  const UInt128 Rem = Mag % Quantum;
  return finishRounding(Value < 0, Qm * Quantum, Quantum, (Qm & 1U) != 0,
                        fracFromRemainder(Rem, Quantum), Mode);
}

std::string u128ToString(UInt128 V) {
  if (V == 0)
    return "0";
  std::string S;
  while (V != 0) {
    S.push_back(static_cast<char>('0' + static_cast<int>(V % 10)));
    V /= 10;
  }
  std::reverse(S.begin(), S.end());
  return S;
}

enum class ParseStatus { Ok, Invalid, Overflow };

ParseStatus parseRaw(std::string_view Text, Decimal::Raw &Out) {
  std::size_t I = 0;
  bool Negative = false;
  if (I < Text.size() && Text[I] == '-') {
    Negative = true;
    ++I;
  }
  constexpr UInt128 kMaxInt = kMaxMag / kUnit;
  UInt128 Integer = 0;
  std::size_t IntDigits = 0;
  bool Overflow = false;
  for (; I < Text.size() && Text[I] >= '0' && Text[I] <= '9';
       ++I, ++IntDigits) {
    const UInt128 Digit = static_cast<UInt128>(Text[I] - '0');
    if (Integer > (kMaxInt - Digit) / 10) {
      Overflow = true; // всё равно проверяем остаток строки на корректность
      Integer = 0;
    } else {
      Integer = Integer * 10 + Digit;
    }
  }
  if (IntDigits == 0)
    return ParseStatus::Invalid;
  UInt128 Fraction = 0;
  std::size_t FracDigits = 0;
  if (I < Text.size() && Text[I] == '.') {
    ++I;
    for (; I < Text.size() && Text[I] >= '0' && Text[I] <= '9';
         ++I, ++FracDigits) {
      if (FracDigits >= static_cast<std::size_t>(Decimal::kScale))
        return ParseStatus::Invalid;
      Fraction = Fraction * 10 + static_cast<UInt128>(Text[I] - '0');
    }
    if (FracDigits == 0)
      return ParseStatus::Invalid;
    for (std::size_t K = FracDigits;
         K < static_cast<std::size_t>(Decimal::kScale); ++K)
      Fraction *= 10;
  }
  if (I != Text.size())
    return ParseStatus::Invalid;
  if (Overflow)
    return ParseStatus::Overflow;
  const UInt128 Mag = Integer * kUnit + Fraction;
  if (Mag > kMaxMag)
    return ParseStatus::Overflow;
  const auto V = static_cast<Decimal::Raw>(Mag);
  Out = Negative ? -V : V;
  return ParseStatus::Ok;
}

} // namespace

Decimal Decimal::one() { return fromUnits(1); }

Decimal Decimal::fromUnits(std::int64_t Units) {
  Decimal D;
  D.RawValue = static_cast<Raw>(Units) * kUnit;
  return D;
}

Decimal Decimal::fromUnitsNano(std::int64_t Units, std::int32_t Nano) {
  if (Nano <= -kUnit || Nano >= kUnit) {
    throw std::invalid_argument("Decimal: |nano| must be <= 999999999");
  }
  if ((Units > 0 && Nano < 0) || (Units < 0 && Nano > 0)) {
    throw std::invalid_argument(
        "Decimal: nano must have the same sign as units");
  }
  Decimal D;
  D.RawValue = static_cast<Raw>(Units) * kUnit + Nano;
  return D;
}

Decimal Decimal::fromRaw(Raw Value) {
  if (Value < -kMaxRaw)
    throwOverflow();
  Decimal D;
  D.RawValue = Value;
  return D;
}

Decimal Decimal::parse(std::string_view Text) {
  Raw RawValue = 0;
  switch (parseRaw(Text, RawValue)) {
  case ParseStatus::Ok:
    return fromRaw(RawValue);
  case ParseStatus::Overflow:
    throwOverflow();
  case ParseStatus::Invalid:
    break;
  }
  throw std::invalid_argument("Decimal: invalid number \"" + std::string(Text) +
                              "\"");
}

std::optional<Decimal> Decimal::tryParse(std::string_view Text) noexcept {
  Raw RawValue = 0;
  if (parseRaw(Text, RawValue) != ParseStatus::Ok)
    return std::nullopt;
  return fromRaw(RawValue);
}

std::string Decimal::toString(int MinFractionDigits) const {
  checkScale(MinFractionDigits);
  const UInt128 Mag = magnitude(RawValue);
  std::string Out;
  if (RawValue < 0)
    Out.push_back('-');
  Out += u128ToString(Mag / kUnit);
  std::string Frac = u128ToString(Mag % kUnit);
  Frac.insert(0, static_cast<std::size_t>(kScale) - Frac.size(), '0');
  while (Frac.size() > static_cast<std::size_t>(MinFractionDigits) &&
         Frac.back() == '0') {
    Frac.pop_back();
  }
  if (!Frac.empty()) {
    Out.push_back('.');
    Out += Frac;
  }
  return Out;
}

UnitsNano Decimal::toUnitsNano() const {
  const Raw Units = RawValue / kUnit;
  if (Units > std::numeric_limits<std::int64_t>::max() ||
      Units < std::numeric_limits<std::int64_t>::min()) {
    throwOverflow();
  }
  // Остаток от деления берёт знак делимого: знак nano совпадает со знаком units
  // (или units = 0).
  return UnitsNano{static_cast<std::int64_t>(Units),
                   static_cast<std::int32_t>(RawValue % kUnit)};
}

double Decimal::toDouble() const { return static_cast<double>(RawValue) / 1e9; }

Decimal Decimal::operator-() const {
  Decimal D;
  D.RawValue = -RawValue; // диапазон симметричен, переполнения нет
  return D;
}

Decimal Decimal::abs() const { return RawValue < 0 ? -*this : *this; }

Decimal operator+(const Decimal &A, const Decimal &B) {
  Decimal::Raw R = 0;
  if (__builtin_add_overflow(A.RawValue, B.RawValue, &R))
    throwOverflow();
  return Decimal::fromRaw(R);
}

Decimal operator-(const Decimal &A, const Decimal &B) {
  Decimal::Raw R = 0;
  if (__builtin_sub_overflow(A.RawValue, B.RawValue, &R))
    throwOverflow();
  return Decimal::fromRaw(R);
}

Decimal Decimal::times(std::int64_t N) const {
  Raw R = 0;
  if (__builtin_mul_overflow(RawValue, static_cast<Raw>(N), &R))
    throwOverflow();
  return fromRaw(R);
}

Decimal Decimal::mul(const Decimal &B, int Scale, RoundingMode Mode) const {
  checkScale(Scale);
  return fromRaw(mulDivRound(RawValue, B.RawValue, static_cast<Raw>(kUnit),
                             pow10ForScale(Scale), Mode));
}

Decimal Decimal::div(const Decimal &B, int Scale, RoundingMode Mode) const {
  checkScale(Scale);
  return fromRaw(mulDivRound(RawValue, static_cast<Raw>(kUnit), B.RawValue,
                             pow10ForScale(Scale), Mode));
}

Decimal Decimal::mulDiv(const Decimal &MulBy, const Decimal &DivBy, int Scale,
                        RoundingMode Mode) const {
  checkScale(Scale);
  return fromRaw(mulDivRound(RawValue, MulBy.RawValue, DivBy.RawValue,
                             pow10ForScale(Scale), Mode));
}

Decimal Decimal::round(int Scale, RoundingMode Mode) const {
  checkScale(Scale);
  return fromRaw(roundToMultiple(RawValue, pow10ForScale(Scale), Mode));
}

Decimal Decimal::roundToStep(const Decimal &Step, RoundingMode Mode) const {
  if (Step.RawValue <= 0)
    throw std::invalid_argument("Decimal: price step must be positive");
  return fromRaw(roundToMultiple(RawValue, magnitude(Step.RawValue), Mode));
}

std::ostream &operator<<(std::ostream &Os, const Decimal &D) {
  return Os << D.toString();
}

} // namespace core
