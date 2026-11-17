#pragma once

#include <string>
#include <vector>

#include "core/dsl/ast.hpp"
#include "core/dsl/checker.hpp"
#include "core/dsl/diagnostic.hpp"

// Стратегия из исходника для обработчиков прогонов (RunBacktest,
// RunWalkForward). Чистая функция (INV-4).

namespace worker {

// Разобранная и проверенная стратегия: программа, параметры по check() и
// уровень версии по статической проверке.
struct Strategy {
  core::dsl::Program Program;
  std::vector<core::dsl::ResolvedParam> Params;
  core::dsl::Level Severity = core::dsl::Level::Ok;
};

// Коды диагностик уровня Error через ';' (контракт simulation.proto: описание
// INVALID_ARGUMENT).
std::string errorCodes(const std::vector<core::dsl::Diagnostic> &Diagnostics);

// parse + check. Ошибка разбора или проверки (уровень Error) -
// std::invalid_argument с кодами диагностик (statusFromCurrentException
// переводит в INVALID_ARGUMENT).
Strategy loadStrategy(const std::string &Source);

} // namespace worker
