#pragma once

#include <cstdint>

#include "core/money/decimal.hpp"

namespace core {

// Бар бумаги на шаге общей шкалы времени сессии. Объём - в лотах (целых, округление вниз).
struct StepBar {
  std::int64_t step = 0;
  int instrument = 0;                // индекс в Timeline::instruments()
  std::int64_t timeUnixMs = 0;       // начало бара
  std::int32_t tradingDay = 0;       // номер торгового дня шкалы, с 1
  Decimal open;
  Decimal high;
  Decimal low;
  Decimal close;
  std::int64_t volumeLots = 0;
  bool afterHalt = false;            // перед баром был перерыв в торгах бумагой (ФТ-5)

  friend bool operator==(const StepBar&, const StepBar&) = default;
};

}  // namespace core
