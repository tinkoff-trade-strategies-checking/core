#include "core/money/decimal.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <ostream>

namespace core {
namespace {

constexpr UInt128 kMaxMag = static_cast<UInt128>(Decimal::kMaxRaw);
constexpr UInt128 kUnit = static_cast<UInt128>(Decimal::kUnit);

constexpr UInt128 pow10ForScale(int scale) {
  // Размер кванта в единицах хранения: 10^(9 - scale).
  UInt128 m = 1;
  for (int i = scale; i < Decimal::kScale; ++i)
    m *= 10;
  return m;
}

void checkScale(int scale) {
  if (scale < 0 || scale > Decimal::kScale) {
    throw std::invalid_argument("Decimal: scale must be in [0, 9]");
  }
}

[[noreturn]] void throwOverflow() {
  throw DecimalOverflow("Decimal: overflow");
}

UInt128 magnitude(Decimal::Raw r) {
  return r < 0 ? static_cast<UInt128>(-r) : static_cast<UInt128>(r);
}

Decimal::Raw withSign(bool negative, UInt128 mag) {
  if (mag > kMaxMag)
    throwOverflow();
  const auto v = static_cast<Decimal::Raw>(mag);
  return negative ? -v : v;
}

// Дробная часть отбрасываемого остатка относительно кванта.
enum class Frac { Zero, Below, Half, Above };

Frac fracFromRemainder(UInt128 rem, UInt128 quantum) {
  if (rem == 0)
    return Frac::Zero;
  const UInt128 other = quantum - rem;
  if (rem < other)
    return Frac::Below;
  if (rem == other)
    return Frac::Half;
  return Frac::Above;
}

// lower - модуль, усечённый к кратному кванту; lowerQuotientOdd - чётность
// lower / quantum (нужна HALF_EVEN). Возвращает окончательное значение со
// знаком.
Decimal::Raw finishRounding(bool negative, UInt128 lower, UInt128 quantum,
                            bool lowerQuotientOdd, Frac frac,
                            RoundingMode mode) {
  bool increment = false;
  switch (mode) {
  case RoundingMode::UP:
    increment = frac != Frac::Zero;
    break;
  case RoundingMode::DOWN:
    break;
  case RoundingMode::CEILING:
    increment = !negative && frac != Frac::Zero;
    break;
  case RoundingMode::FLOOR:
    increment = negative && frac != Frac::Zero;
    break;
  case RoundingMode::HALF_UP:
    increment = frac == Frac::Half || frac == Frac::Above;
    break;
  case RoundingMode::HALF_DOWN:
    increment = frac == Frac::Above;
    break;
  case RoundingMode::HALF_EVEN:
    increment = frac == Frac::Above || (frac == Frac::Half && lowerQuotientOdd);
    break;
  case RoundingMode::UNNECESSARY:
    if (frac != Frac::Zero)
      throw std::domain_error("Decimal: rounding necessary");
    break;
  }
  if (lower > kMaxMag)
    throwOverflow();
  if (increment) {
    if (quantum > kMaxMag - lower)
      throwOverflow();
    lower += quantum;
  }
  return withSign(negative, lower);
}

// ---- 256-битная арифметика для умножения и деления без потерь
// ------------------------
struct U256 {
  std::array<std::uint64_t, 4> w{}; // little-endian
};

U256 mul128(UInt128 a, UInt128 b) {
  const std::uint64_t a0 = static_cast<std::uint64_t>(a);
  const std::uint64_t a1 = static_cast<std::uint64_t>(a >> 64);
  const std::uint64_t b0 = static_cast<std::uint64_t>(b);
  const std::uint64_t b1 = static_cast<std::uint64_t>(b >> 64);
  const UInt128 p00 = static_cast<UInt128>(a0) * b0;
  const UInt128 p01 = static_cast<UInt128>(a0) * b1;
  const UInt128 p10 = static_cast<UInt128>(a1) * b0;
  const UInt128 p11 = static_cast<UInt128>(a1) * b1;
  const UInt128 mid = (p00 >> 64) + static_cast<std::uint64_t>(p01) +
                      static_cast<std::uint64_t>(p10);
  const UInt128 hi = (mid >> 64) + (p01 >> 64) + (p10 >> 64) + p11;
  U256 r;
  r.w[0] = static_cast<std::uint64_t>(p00);
  r.w[1] = static_cast<std::uint64_t>(mid);
  r.w[2] = static_cast<std::uint64_t>(hi);
  r.w[3] = static_cast<std::uint64_t>(hi >> 64);
  return r;
}

struct DivResult {
  U256 quotient;
  UInt128 remainder = 0;
};

// n / d, d > 0 и d <= kMaxMag (поэтому сдвиг остатка не переполняет 128 бит).
DivResult divide(const U256 &n, UInt128 d) {
  DivResult res;
  if (n.w[2] == 0 && n.w[3] == 0) {
    const UInt128 low = (static_cast<UInt128>(n.w[1]) << 64) | n.w[0];
    const UInt128 q = low / d;
    res.remainder = low % d;
    res.quotient.w[0] = static_cast<std::uint64_t>(q);
    res.quotient.w[1] = static_cast<std::uint64_t>(q >> 64);
    return res;
  }
  int top = 255;
  while (top >= 0 && ((n.w[top / 64] >> (top % 64)) & 1U) == 0)
    --top;
  UInt128 rem = 0;
  for (int i = top; i >= 0; --i) {
    rem = (rem << 1) | ((n.w[i / 64] >> (i % 64)) & 1U);
    if (rem >= d) {
      rem -= d;
      res.quotient.w[i / 64] |= std::uint64_t{1} << (i % 64);
    }
  }
  res.remainder = rem;
  return res;
}

// a * b / c в единицах хранения, результат округляется до кратного quantum (>=
// 1) одним шагом.
Decimal::Raw mulDivRound(Decimal::Raw a, Decimal::Raw b, Decimal::Raw c,
                         UInt128 quantum, RoundingMode mode) {
  if (c == 0)
    throw std::domain_error("Decimal: division by zero");
  if (a == 0 || b == 0)
    return 0;
  const bool negative = ((a < 0) != (b < 0)) != (c < 0);
  const UInt128 d = magnitude(c);
  const DivResult dr = divide(mul128(magnitude(a), magnitude(b)), d);
  if (dr.quotient.w[2] != 0 || dr.quotient.w[3] != 0)
    throwOverflow();
  const UInt128 q =
      (static_cast<UInt128>(dr.quotient.w[1]) << 64) | dr.quotient.w[0];

  const UInt128 qm = q / quantum;
  const UInt128 qr = q % quantum;
  Frac frac;
  if (qr == 0 && dr.remainder == 0) {
    frac = Frac::Zero;
  } else if (quantum == 1) {
    // Остаток деления r / d в (0, 1): сравниваем 2r с d (2r < 2^128, так как r
    // < d <= 2^127).
    const UInt128 twice = dr.remainder * 2;
    frac = twice < d ? Frac::Below : (twice == d ? Frac::Half : Frac::Above);
  } else {
    const UInt128 half =
        quantum / 2; // квант - степень десяти >= 10, делится на 2
    if (qr < half) {
      frac = Frac::Below;
    } else if (qr > half) {
      frac = Frac::Above;
    } else {
      frac = dr.remainder == 0 ? Frac::Half : Frac::Above;
    }
  }
  return finishRounding(negative, qm * quantum, quantum, (qm & 1U) != 0, frac,
                        mode);
}

Decimal::Raw roundToMultiple(Decimal::Raw value, UInt128 quantum,
                             RoundingMode mode) {
  const UInt128 mag = magnitude(value);
  const UInt128 qm = mag / quantum;
  const UInt128 rem = mag % quantum;
  return finishRounding(value < 0, qm * quantum, quantum, (qm & 1U) != 0,
                        fracFromRemainder(rem, quantum), mode);
}

std::string u128ToString(UInt128 v) {
  if (v == 0)
    return "0";
  std::string s;
  while (v != 0) {
    s.push_back(static_cast<char>('0' + static_cast<int>(v % 10)));
    v /= 10;
  }
  std::reverse(s.begin(), s.end());
  return s;
}

enum class ParseStatus { Ok, Invalid, Overflow };

ParseStatus parseRaw(std::string_view text, Decimal::Raw &out) {
  std::size_t i = 0;
  bool negative = false;
  if (i < text.size() && text[i] == '-') {
    negative = true;
    ++i;
  }
  constexpr UInt128 kMaxInt = kMaxMag / kUnit;
  UInt128 integer = 0;
  std::size_t intDigits = 0;
  bool overflow = false;
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9';
       ++i, ++intDigits) {
    const UInt128 digit = static_cast<UInt128>(text[i] - '0');
    if (integer > (kMaxInt - digit) / 10) {
      overflow = true; // всё равно проверяем остаток строки на корректность
      integer = 0;
    } else {
      integer = integer * 10 + digit;
    }
  }
  if (intDigits == 0)
    return ParseStatus::Invalid;
  UInt128 fraction = 0;
  std::size_t fracDigits = 0;
  if (i < text.size() && text[i] == '.') {
    ++i;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9';
         ++i, ++fracDigits) {
      if (fracDigits >= static_cast<std::size_t>(Decimal::kScale))
        return ParseStatus::Invalid;
      fraction = fraction * 10 + static_cast<UInt128>(text[i] - '0');
    }
    if (fracDigits == 0)
      return ParseStatus::Invalid;
    for (std::size_t k = fracDigits;
         k < static_cast<std::size_t>(Decimal::kScale); ++k)
      fraction *= 10;
  }
  if (i != text.size())
    return ParseStatus::Invalid;
  if (overflow)
    return ParseStatus::Overflow;
  const UInt128 mag = integer * kUnit + fraction;
  if (mag > kMaxMag)
    return ParseStatus::Overflow;
  const auto v = static_cast<Decimal::Raw>(mag);
  out = negative ? -v : v;
  return ParseStatus::Ok;
}

} // namespace

Decimal Decimal::one() { return fromUnits(1); }

Decimal Decimal::fromUnits(std::int64_t units) {
  Decimal d;
  d.raw_ = static_cast<Raw>(units) * kUnit;
  return d;
}

Decimal Decimal::fromUnitsNano(std::int64_t units, std::int32_t nano) {
  if (nano <= -kUnit || nano >= kUnit) {
    throw std::invalid_argument("Decimal: |nano| must be <= 999999999");
  }
  if ((units > 0 && nano < 0) || (units < 0 && nano > 0)) {
    throw std::invalid_argument(
        "Decimal: nano must have the same sign as units");
  }
  Decimal d;
  d.raw_ = static_cast<Raw>(units) * kUnit + nano;
  return d;
}

Decimal Decimal::fromRaw(Raw raw) {
  if (raw < -kMaxRaw)
    throwOverflow();
  Decimal d;
  d.raw_ = raw;
  return d;
}

Decimal Decimal::parse(std::string_view text) {
  Raw raw = 0;
  switch (parseRaw(text, raw)) {
  case ParseStatus::Ok:
    return fromRaw(raw);
  case ParseStatus::Overflow:
    throwOverflow();
  case ParseStatus::Invalid:
    break;
  }
  throw std::invalid_argument("Decimal: invalid number \"" + std::string(text) +
                              "\"");
}

std::optional<Decimal> Decimal::tryParse(std::string_view text) noexcept {
  Raw raw = 0;
  if (parseRaw(text, raw) != ParseStatus::Ok)
    return std::nullopt;
  return fromRaw(raw);
}

std::string Decimal::toString(int minFractionDigits) const {
  checkScale(minFractionDigits);
  const UInt128 mag = magnitude(raw_);
  std::string out;
  if (raw_ < 0)
    out.push_back('-');
  out += u128ToString(mag / kUnit);
  std::string frac = u128ToString(mag % kUnit);
  frac.insert(0, static_cast<std::size_t>(kScale) - frac.size(), '0');
  while (frac.size() > static_cast<std::size_t>(minFractionDigits) &&
         frac.back() == '0') {
    frac.pop_back();
  }
  if (!frac.empty()) {
    out.push_back('.');
    out += frac;
  }
  return out;
}

UnitsNano Decimal::toUnitsNano() const {
  const Raw units = raw_ / kUnit;
  if (units > std::numeric_limits<std::int64_t>::max() ||
      units < std::numeric_limits<std::int64_t>::min()) {
    throwOverflow();
  }
  // Остаток от деления берёт знак делимого: знак nano совпадает со знаком units
  // (или units = 0).
  return UnitsNano{static_cast<std::int64_t>(units),
                   static_cast<std::int32_t>(raw_ % kUnit)};
}

double Decimal::toDouble() const { return static_cast<double>(raw_) / 1e9; }

Decimal Decimal::operator-() const {
  Decimal d;
  d.raw_ = -raw_; // диапазон симметричен, переполнения нет
  return d;
}

Decimal Decimal::abs() const { return raw_ < 0 ? -*this : *this; }

Decimal operator+(const Decimal &a, const Decimal &b) {
  Decimal::Raw r = 0;
  if (__builtin_add_overflow(a.raw_, b.raw_, &r))
    throwOverflow();
  return Decimal::fromRaw(r);
}

Decimal operator-(const Decimal &a, const Decimal &b) {
  Decimal::Raw r = 0;
  if (__builtin_sub_overflow(a.raw_, b.raw_, &r))
    throwOverflow();
  return Decimal::fromRaw(r);
}

Decimal Decimal::times(std::int64_t n) const {
  Raw r = 0;
  if (__builtin_mul_overflow(raw_, static_cast<Raw>(n), &r))
    throwOverflow();
  return fromRaw(r);
}

Decimal Decimal::mul(const Decimal &b, int scale, RoundingMode mode) const {
  checkScale(scale);
  return fromRaw(mulDivRound(raw_, b.raw_, static_cast<Raw>(kUnit),
                             pow10ForScale(scale), mode));
}

Decimal Decimal::div(const Decimal &b, int scale, RoundingMode mode) const {
  checkScale(scale);
  return fromRaw(mulDivRound(raw_, static_cast<Raw>(kUnit), b.raw_,
                             pow10ForScale(scale), mode));
}

Decimal Decimal::mulDiv(const Decimal &mulBy, const Decimal &divBy, int scale,
                        RoundingMode mode) const {
  checkScale(scale);
  return fromRaw(
      mulDivRound(raw_, mulBy.raw_, divBy.raw_, pow10ForScale(scale), mode));
}

Decimal Decimal::round(int scale, RoundingMode mode) const {
  checkScale(scale);
  return fromRaw(roundToMultiple(raw_, pow10ForScale(scale), mode));
}

Decimal Decimal::roundToStep(const Decimal &step, RoundingMode mode) const {
  if (step.raw_ <= 0)
    throw std::invalid_argument("Decimal: price step must be positive");
  return fromRaw(roundToMultiple(raw_, magnitude(step.raw_), mode));
}

std::ostream &operator<<(std::ostream &os, const Decimal &d) {
  return os << d.toString();
}

} // namespace core
