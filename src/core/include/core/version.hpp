#pragma once

#include <string_view>

namespace core {

// Версия Simulation Core (совпадает с версией проекта в CMake).
std::string_view version() noexcept;

} // namespace core
