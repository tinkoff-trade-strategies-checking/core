#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/dsl/ast.hpp"
#include "core/dsl/diagnostic.hpp"

// Проверка разобранной программы (dsl.md, «Типы и значения», «Данные», «Диагностики», «Чтение будущего»).
// Статическая, до запуска (INV-1): типы, имена, параметры, пределы смещений и периодов, чтение будущего.

namespace core::dsl {

// Параметр после проверки. integer - параметр стоит в целочисленной позиции (период индикатора или
// смещение), значит, и значение, и диапазон, и шаг целые; переопределённое значение тоже должно быть целым.
struct ResolvedParam {
  std::string name;
  double value;
  std::optional<double> min, max, step;
  bool integer;
};

struct CheckResult {
  Level level;  // худший уровень диагностик; Ok, если их нет
  std::vector<Diagnostic> diagnostics;  // в порядке обхода текста: параметры, затем правила
  std::vector<ResolvedParam> params;    // в порядке объявления; повторные объявления пропущены
};

// Коды: E-TYPE (тип, имя, параметры, периоды, пределы), E-BEC (пустая причина), E-LA01, E-LA02, W-LA01.
// Пределы: смещение и период - не больше 5000 баров (dsl.md, «Где живёт»). Чистая функция.
CheckResult check(const Program& program);

// Худший уровень: Error > Warning > Ok. Без диагностик - Ok.
Level overallLevel(const std::vector<Diagnostic>& diagnostics);

}  // namespace core::dsl
