#include "core/market/timeframe.hpp"

#include <stdexcept>

namespace core {

std::int32_t intervalSeconds(Timeframe timeframe) {
  switch (timeframe) {
  case Timeframe::S1:
    return 1;
  case Timeframe::S10:
    return 10;
  case Timeframe::M1:
    return 60;
  case Timeframe::M5:
    return 300;
  case Timeframe::M10:
    return 600;
  case Timeframe::M15:
    return 900;
  case Timeframe::H1:
    return 3600;
  case Timeframe::D1:
    return 86400;
  }
  throw std::invalid_argument("неизвестный таймфрейм");
}

} // namespace core
