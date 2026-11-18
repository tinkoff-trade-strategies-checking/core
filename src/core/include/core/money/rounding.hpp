#pragma once

namespace core {

// Режимы округления. Названия и смысл — как в java.math.RoundingMode
// (`decisions.md`, «Деньги в C++»), чтобы результат ядра совпадал с Java до
// копейки (INV-6). UP, DOWN, HALF_* считаются по модулю числа («от нуля», «к
// нулю»); CEILING и FLOOR — по знаку.
enum class RoundingMode {
  UP,      // от нуля
  DOWN,    // к нулю (усечение)
  CEILING, // к +бесконечности
  FLOOR,   // к -бесконечности
  HalfUp, // к ближайшему, ровно половина - от нуля
  HalfDown, // к ближайшему, ровно половина - к нулю
  HalfEven, // к ближайшему, ровно половина - к чётному
  UNNECESSARY, // округление не требуется; если требуется - исключение
};

} // namespace core
