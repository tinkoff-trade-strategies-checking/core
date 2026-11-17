#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace core::dsl {

// Значение ряда DSL: nullopt — `na`. Логические значения — 1.0 (true) и 0.0
// (false).
using Value = std::optional<double>;

// Индикатор по формулам dsl.md, раздел «Функции». update() вызывается один раз
// на бар. Если на входе есть `na`, состояние не меняется, результат — `na`.
// Только + − × ÷ и sqrt, порядок суммирования фиксирован (INV-4).
class Indicator {
public:
  virtual ~Indicator() = default;
  virtual Value update(const std::vector<Value> &Inputs) = 0;
};

// Имена и входы:
//   sma ema rsi stdev highest lowest change — входы {x}, intArgs {n};
//   atr — входы {high, low, close}, intArgs {n};
//   crossover crossunder — входы {a, b}, intArgs {}.
// Неизвестное имя, неверное число или значение аргументов —
// std::invalid_argument. update с неверным числом входов тоже бросает
// std::invalid_argument.
std::unique_ptr<Indicator>
makeIndicator(std::string_view Name, const std::vector<std::int64_t> &IntArgs);

} // namespace core::dsl
