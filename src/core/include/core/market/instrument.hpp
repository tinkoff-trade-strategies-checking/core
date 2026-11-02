#pragma once

#include <cstdint>
#include <string>

#include "core/money/decimal.hpp"

namespace core {

// Бумага сессии. key - ключ сессии ("A"…"E", порядок обработки задан порядком в списке),
// ticker - secid в источнике данных.
struct Instrument {
  std::string key;
  std::string ticker;
  std::int64_t lotSize = 1;
  Decimal priceStep;

  friend bool operator==(const Instrument&, const Instrument&) = default;
};

}  // namespace core
