#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "core/money/rounding.hpp"

// Деньги и цены ядра (ADR-0008, INV-6): число - целое 128 бит в единицах 10^-9,
// то есть ровно 9 знаков после запятой. Никакого double: ни конструктора, ни операций.
// Все операции детерминированы и не зависят от платформы (INV-4).
//
// Ошибки - исключения (ADR: «переполнение - исключение, прогон завершается ошибкой»):
//   DecimalOverflow        - результат не помещается в диапазон;
//   std::invalid_argument  - неверная строка, масштаб, шаг, знаки units/nano;
//   std::domain_error      - деление на ноль; режим UNNECESSARY, когда округление нужно.

namespace core {

// Внутреннее представление. __int128 - расширение GCC/Clang (целевая сборка - Linux/Docker);
// __extension__ нужен, чтобы -Wpedantic -Werror не срабатывал.
__extension__ typedef __int128 Int128;
__extension__ typedef unsigned __int128 UInt128;

class DecimalOverflow : public std::overflow_error {
 public:
  using std::overflow_error::overflow_error;
};

// Значение в виде пары полей контракта `.proto` Decimal{units, nano} (знаки - как в ADR-0008).
struct UnitsNano {
  std::int64_t units = 0;
  std::int32_t nano = 0;
  friend bool operator==(const UnitsNano&, const UnitsNano&) = default;
};

class Decimal {
 public:
  using Raw = Int128;
  static constexpr int kScale = 9;                      // знаков после запятой
  static constexpr std::int64_t kUnit = 1000000000;     // 10^9 единиц хранения в 1.0
  // Допустимый диапазон Raw симметричен: +-(2^127 - 1). Минимум int128 не используется.
  static constexpr Raw kMaxRaw = static_cast<Raw>((static_cast<UInt128>(1) << 127) - 1);

  // ---- Создание --------------------------------------------------------------------
  constexpr Decimal() = default;  // 0

  // Из чисел с плавающей точкой строить нельзя (INV-6): ошибка компиляции.
  template <std::floating_point F>
  Decimal(F) = delete;
  template <std::floating_point F>
  static Decimal fromUnits(F) = delete;

  static Decimal zero() { return Decimal(); }
  static Decimal one();
  // Целое число рублей (без дробной части).
  static Decimal fromUnits(std::int64_t units);
  // Из пары контракта .proto: значение = units + nano * 10^-9. |nano| <= 999 999 999, знак nano
  // совпадает со знаком units (или один из них 0), иначе std::invalid_argument.
  static Decimal fromUnitsNano(std::int64_t units, std::int32_t nano);
  // Из числа единиц 10^-9 (Raw). Минимум int128 недопустим -> DecimalOverflow.
  static Decimal fromRaw(Raw raw);
  // Точный разбор: [-]цифры[.цифры], не более 9 знаков после запятой, без пробелов и «+».
  // Неверная строка - std::invalid_argument, слишком большое число - DecimalOverflow.
  static Decimal parse(std::string_view text);
  // То же без исключений: nullopt при любой ошибке.
  static std::optional<Decimal> tryParse(std::string_view text) noexcept;

  // ---- Просмотр --------------------------------------------------------------------
  constexpr Raw raw() const { return raw_; }
  constexpr bool isZero() const { return raw_ == 0; }
  constexpr int sign() const { return raw_ > 0 ? 1 : (raw_ < 0 ? -1 : 0); }
  // Каноническая строка: «-», целая часть, при ненулевой дробной - «.» и знаки без хвостовых
  // нулей ("123.45", "-0.000000001", "5"). minFractionDigits (0...9) добивает дробную часть
  // нулями: toString(2) -> "5.00" (удобно для копеек).
  std::string toString(int minFractionDigits = 0) const;
  // Пара для .proto; units вне int64 -> DecimalOverflow.
  UnitsNano toUnitsNano() const;
  // Только для метрик и выражений DSL (ADR-0008); деньги обратно из double не строятся.
  double toDouble() const;

  // ---- Точная арифметика (переполнение -> DecimalOverflow) ----------------------------
  Decimal operator-() const;
  Decimal abs() const;
  friend Decimal operator+(const Decimal& a, const Decimal& b);
  friend Decimal operator-(const Decimal& a, const Decimal& b);
  Decimal& operator+=(const Decimal& b) { return *this = *this + b; }
  Decimal& operator-=(const Decimal& b) { return *this = *this - b; }
  // Умножение на целое (лоты, размер лота) - точное.
  Decimal times(std::int64_t n) const;
  template <std::floating_point F>
  Decimal times(F) const = delete;

  // ---- Умножение и деление: режим округления указывается всегда -----------------------
  // Результат - число со `scale` знаками после запятой (0...9, по умолчанию 9), округлённое
  // из ТОЧНОГО значения за один шаг (без двойного округления). Промежуточные значения
  // считаются без потерь (256 бит).
  Decimal mul(const Decimal& b, RoundingMode mode) const { return mul(b, kScale, mode); }
  Decimal mul(const Decimal& b, int scale, RoundingMode mode) const;
  Decimal div(const Decimal& b, RoundingMode mode) const { return div(b, kScale, mode); }
  Decimal div(const Decimal& b, int scale, RoundingMode mode) const;
  // this * mulBy / divBy одним шагом (слепой масштаб: цена * 100 / P0, `decisions.md`, п. 8).
  Decimal mulDiv(const Decimal& mulBy, const Decimal& divBy, int scale, RoundingMode mode) const;

  // ---- Округление --------------------------------------------------------------------
  // До `scale` знаков после запятой (0...9); сумма до копейки - round(2, mode).
  Decimal round(int scale, RoundingMode mode) const;
  // До кратного шагу (step > 0): цена до шага цены. Кратность считается от нуля.
  // Не округляй дважды разными режимами (mul HALF_UP, затем шаг CEILING): в mul бери тот же режим.
  Decimal roundToStep(const Decimal& step, RoundingMode mode) const;

  // ---- Сравнение ---------------------------------------------------------------------
  friend constexpr bool operator==(const Decimal& a, const Decimal& b) { return a.raw_ == b.raw_; }
  friend constexpr std::strong_ordering operator<=>(const Decimal& a, const Decimal& b) {
    return a.raw_ <=> b.raw_;
  }

 private:
  Raw raw_ = 0;
};

std::ostream& operator<<(std::ostream& os, const Decimal& d);

}  // namespace core
