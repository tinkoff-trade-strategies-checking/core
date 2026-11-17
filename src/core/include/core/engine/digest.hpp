#pragma once

#include <cstdint>

#include "core/engine/session_engine.hpp"

// Отпечаток состояния сессии (INV-4): один и тот же вход и журнал дают один и
// тот же отпечаток.

namespace core {

// FNV-1a 64 по каноническому тексту сессии: сделки, точки кривой стоимости,
// статус.
std::uint64_t digestOf(const SessionEngine &Engine);

} // namespace core
