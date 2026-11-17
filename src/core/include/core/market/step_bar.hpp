#pragma once

#include <cstdint>

#include "core/money/decimal.hpp"

namespace core {

// Бар бумаги на шаге общей шкалы времени сессии. Объём - в лотах (целых,
// округление вниз).
struct StepBar {
  std::int64_t Step = 0;
  int Instrument = 0;          // индекс в Timeline::instruments()
  std::int64_t TimeUnixMs = 0; // начало бара
  std::int32_t TradingDay = 0; // номер торгового дня шкалы, с 1
  Decimal Open;
  Decimal High;
  Decimal Low;
  Decimal Close;
  std::int64_t VolumeLots = 0;
  bool AfterHalt = false; // перед баром был перерыв в торгах бумагой (ФТ-5)

  friend bool operator==(const StepBar &, const StepBar &) = default;
};

} // namespace core
