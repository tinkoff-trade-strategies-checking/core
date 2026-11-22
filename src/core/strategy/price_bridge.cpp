#include "core/strategy/price_bridge.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "dsl/types.hpp"

namespace core {
namespace {

constexpr std::uint64_t pow10U64(int Exp) {
  std::uint64_t R = 1;
  for (int I = 0; I < Exp; ++I) {
    R *= 10;
  }
  return R;
}

} // namespace

Decimal roundDslPriceToStep(double RawPrice, const Decimal &PriceStep) {
  if (!std::isfinite(RawPrice) || RawPrice <= 0.0 ||
      RawPrice > dsl::kMaxDecimalPrice) {
    throw std::invalid_argument(
        "roundDslPriceToStep: цена должна быть конечной в (0, 1e15]");
  }
  if (PriceStep.sign() <= 0) {
    throw std::invalid_argument(
        "roundDslPriceToStep: шаг цены должен быть положительным");
  }

  std::array<char, 64> Buf{};
  const auto Res = std::to_chars(Buf.data(), Buf.data() + Buf.size(), RawPrice);
  if (Res.ec != std::errc{}) {
    throw std::invalid_argument(
        "roundDslPriceToStep: не удалось преобразовать цену в текст");
  }

  const std::string_view Str(Buf.data(),
                             static_cast<std::size_t>(Res.ptr - Buf.data()));
  std::size_t ExpPos = Str.find('e');
  if (ExpPos == std::string_view::npos) {
    ExpPos = Str.find('E');
  }

  std::uint64_t Mantissa = 0;
  int Exp10 = 0;
  bool SeenDot = false;
  const std::size_t MantissaEnd = std::min(ExpPos, Str.size());
  for (std::size_t I = 0; I < MantissaEnd; ++I) {
    const char Ch = Str[I];
    if (Ch == '.') {
      SeenDot = true;
      continue;
    }
    if (Ch < '0' || Ch > '9') {
      throw std::invalid_argument(
          "roundDslPriceToStep: неожиданный символ в представлении цены");
    }
    Mantissa = Mantissa * 10 + static_cast<std::uint64_t>(Ch - '0');
    if (SeenDot) {
      --Exp10;
    }
  }

  if (ExpPos != std::string_view::npos) {
    std::size_t Idx = ExpPos + 1;
    bool NegExp = false;
    if (Idx < Str.size() && (Str[Idx] == '+' || Str[Idx] == '-')) {
      NegExp = (Str[Idx] == '-');
      ++Idx;
    }
    int ExpMag = 0;
    const auto [Ptr, Ec] =
        std::from_chars(Str.data() + Idx, Str.data() + Str.size(), ExpMag);
    if (Ec != std::errc{} || Ptr != Str.data() + Str.size()) {
      throw std::invalid_argument(
          "roundDslPriceToStep: некорректная экспонента цены");
    }
    Exp10 += NegExp ? -ExpMag : ExpMag;
  }

  if (Mantissa == 0) {
    return Decimal::zero();
  }
  while (Mantissa % 10 == 0) {
    Mantissa /= 10;
    ++Exp10;
  }

  if (Exp10 >= -Decimal::kScale) {
    UInt128 Raw = static_cast<UInt128>(Mantissa);
    const int Shift = Decimal::kScale + Exp10;
    for (int I = 0; I < Shift; ++I) {
      Raw *= 10;
    }
    return Decimal::fromRaw(static_cast<Decimal::Raw>(Raw))
        .roundToStep(PriceStep, RoundingMode::HalfUp);
  }

  // Exp10 < -9: точное значение в единицах 10^-9 равно Mantissa / 10^K.
  const int K = -Decimal::kScale - Exp10;
  if (K >= 18) {
    // Mantissa < 10^17 и PriceStep.raw() >= 1 => 2 * Mantissa < PriceStep * 10^K.
    return Decimal::zero();
  }

  const std::uint64_t Pow10K = pow10U64(K);
  const UInt128 StepRaw = static_cast<UInt128>(PriceStep.raw());
  const UInt128 TwiceMantissa = static_cast<UInt128>(Mantissa) * 2;
  if (StepRaw > TwiceMantissa / Pow10K) {
    return Decimal::zero();
  }

  const UInt128 Denom = StepRaw * Pow10K;
  UInt128 Quot = static_cast<UInt128>(Mantissa) / Denom;
  const UInt128 Rem = static_cast<UInt128>(Mantissa) % Denom;
  if (Rem * 2 >= Denom) {
    ++Quot;
  }
  return Decimal::fromRaw(static_cast<Decimal::Raw>(Quot * StepRaw));
}

} // namespace core
