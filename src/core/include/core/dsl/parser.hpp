#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "core/dsl/ast.hpp"
#include "core/dsl/diagnostic.hpp"

namespace core::dsl {

// Результат разбора. Разбор останавливается на первой синтаксической ошибке:
// тогда program пуст, а в diagnostics ровно одна диагностика E-SYN. При успехе
// diagnostics пуст. Ограничения dsl.md (64 КиБ текста, 100 правил, 50
// параметров) тоже дают E-SYN; кроме них - глубина вложенности выражений не
// больше 100, иначе разбор рекурсивных обходов дерева опасен для стека.
struct ParseResult {
  std::optional<Program> Ast;
  std::vector<Diagnostic> Diagnostics;
};

// source - текст в UTF-8. Чистая функция: без состояния и часов.
ParseResult parse(std::string_view Source);

} // namespace core::dsl
