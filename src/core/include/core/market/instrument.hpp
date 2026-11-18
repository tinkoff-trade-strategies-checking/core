#pragma once

#include <cstdint>
#include <string>

#include "core/money/decimal.hpp"

namespace core {

// Бумага сессии. key - ключ сессии ("A"…"E", порядок обработки задан порядком в
// списке), ticker - secid в источнике данных.
struct Instrument {
  std::string Key;
  std::string Ticker;
  std::int64_t LotSize = 1;
  Decimal PriceStep;

  friend bool operator==(const Instrument &, const Instrument &) = default;
  friend auto operator<=>(const Instrument &, const Instrument &) = default;
};

} // namespace core
