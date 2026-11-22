#pragma once

#include <optional>
#include <vector>

#include "core/exec/order.hpp"
#include "core/market/timeline.hpp"
#include "core/portfolio/portfolio.hpp"
#include "core/report/metrics.hpp"
#include "dsl/types.hpp"

// Детекторы чтения будущего и целостности времени исполнения ФТ-14
// (`E-RT01`, `W-RT01`, `W-RT02`). Все контейнеры упорядочены (INV-4).

namespace core {

// Худший из двух уровней проверки: Error > Warning > Ok.
CheckLevel combineCheckLevel(CheckLevel A, CheckLevel B) noexcept;

// Статический уровень чтения будущего по диагностикам DSL: учитывает только
// `E-LA*` (-> Error) и `W-LA*` (-> Warning); `L001` уровень не повышает.
CheckLevel
staticLookaheadLevel(const std::vector<dsl::Diagnostic> &Diagnostics) noexcept;

// `W-RT01` («Угаданные гэпы»): по рыночным исполнениям заявок стратегии с
// ненулевым нетто-объёмом на баре исполнения, если угадано >= 5 гэпов >= 2%
// и они составляют >= 80% таких гэпов.
std::optional<Finding> detectGuessedGaps(const Timeline &Tl,
                                         const std::vector<Order> &Orders,
                                         const std::vector<Fill> &Fills);

// `W-RT02` («Слишком ровный результат»): win rate > 90% при >= 30 закрытых
// сделках по FIFO.
std::optional<Finding>
detectTooSmoothResult(const std::vector<RoundTrip> &RoundTrips);

// Добавляет `ExtraFindings`, `W-RT01` и `W-RT02` в `OutReport.Findings` и
// выставляет `OutReport.ReportLevel` как максимум из `StaticLevel` и всех
// находок отчёта.
void applyRuntimeChecks(Report &OutReport, CheckLevel StaticLevel,
                        const Timeline &Tl, const std::vector<Order> &Orders,
                        const std::vector<Fill> &Fills,
                        const std::vector<Finding> &ExtraFindings = {});

} // namespace core
