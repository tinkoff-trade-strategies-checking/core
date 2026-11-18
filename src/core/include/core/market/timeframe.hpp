#pragma once

#include <cstdint>

// Таймфреймы свечей (ФТ-3).

namespace core {

enum class Timeframe { M1, M5, M10, M15, H1, D1 };

// Длина свечи в секундах: 60, 300, 600, 900, 3600, 86400.
std::int32_t intervalSeconds(Timeframe Timeframe);

} // namespace core
