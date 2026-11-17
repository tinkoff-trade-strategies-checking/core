#pragma once

#include <vector>

#include "core/dsl/checker.hpp"
#include "core/dsl/diagnostic.hpp"
#include "simulation.pb.h"

// Конвертеры DSL <-> proto для CheckStrategy. Чистые функции: без состояния и
// часов (INV-4).

namespace worker::convert {

namespace pb = pac::simulation::v1;

pb::Level toProto(core::dsl::Level level);
pb::Diagnostic toProto(const core::dsl::Diagnostic &diagnostic);
pb::ParamDecl toProto(const core::dsl::ResolvedParam &param);

// Диагностики в детерминированном порядке: по позиции начала (строка, столбец),
// затем по коду; равные по обоим ключам сохраняют исходный порядок (INV-4).
std::vector<core::dsl::Diagnostic>
sortedByPosition(std::vector<core::dsl::Diagnostic> diagnostics);

} // namespace worker::convert
