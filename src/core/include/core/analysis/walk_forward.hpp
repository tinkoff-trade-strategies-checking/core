#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "core/dsl/ast.hpp"
#include "core/dsl/checker.hpp"
#include "core/dsl/interpreter.hpp"
#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "core/report/metrics.hpp"
#include "core/run/backtest.hpp" // RunOutput, Progress

// Walk-forward (ФТ-22, `decisions.md`, «Walk-forward»): период режется на окна
// подбора и проверки со сдвигом в торговых днях шкалы; на окне подбора
// перебирается сетка параметров стратегии, лучший набор проверяется на
// следующем окне. Каждый прогон отдельный, с капиталом из SessionSpec. Прогоны
// идут по очереди, без потоков и случайности (INV-4).

namespace core {

// Ни один набор параметров не дал засчитываемого прогона подбора (все
// остановлены нарушением целостности, E-RT01). Это исход для данной стратегии,
// а не сбой воркера: повтор бессмыслен (воркер отвечает FAILED_PRECONDITION).
struct NoValidFitRun : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct WalkForwardConfig {
  std::int32_t FitDays = 250, TestDays = 63, ShiftDays = 63, MaxGrid = 200;
};

// Номера торговых дней - в исходной шкале, с 1, границы включительно.
struct WalkForwardSegment {
  std::int32_t Index;
  std::int32_t FitFromDay, FitToDay, TestFromDay, TestToDay;
  dsl::ParamValues Chosen; // выбранные значения всех параметров стратегии
  double FitNetReturn; // net-доходность выбранного набора на окне подбора
  RunOutput Test; // прогон на окне проверки; шаги и сделки - в нумерации среза
                  // шкалы (дни testFromDay…testToDay)
  Report TestReport; // без бенчмарка
};

struct WalkForwardResult {
  std::vector<WalkForwardSegment> Segments;
  double ChainedReturn = 0; // по цепочке проверочных окон: Π(1 + r_k) − 1
};

// Сетка: декартово произведение значений параметров в порядке объявления,
// последний параметр меняется быстрее всех. Параметр без диапазона - только
// значение по умолчанию; с диапазоном - от, от + шаг, … <= до. Без параметров -
// одна пустая комбинация. Размер сетки больше maxGrid или maxGrid < 1 -
// std::invalid_argument (до построения сетки).
std::vector<dsl::ParamValues>
paramGrid(const std::vector<dsl::ResolvedParam> &Params, std::int32_t MaxGrid);

// Доходность по цепочке: Π(1 + r_k) − 1; без окон - 0.
double chainedReturn(const std::vector<double> &Returns);

// Отрезок k: подбор [1 + shift·k; fit + shift·k], проверка - следующие testDays
// дней; отрезков столько, сколько помещается в шкалу. Критерий подбора -
// максимальная net-доходность ((итог − капитал) / капитал) на окне подбора, при
// равенстве - лексикографически меньший набор (порядок сетки). Прогон подбора
// со статусом Invalid (E-RT01) не засчитывается; если таких все -
// NoValidFitRun. Уровень версии для прогонов - Ok (как в costBreakdown). Ошибки
// (std::invalid_argument): поля config <= 0, сетка больше maxGrid, ни одного
// отрезка, ошибка runBacktest (в том числе значение сетки, не прошедшее
// повторную проверку StrategyInstance, INV-1). progress - по сумме шагов всех
// прогонов (done монотонно растёт до total); может быть пустым.
WalkForwardResult walkForward(const SessionSpec &Spec,
                              std::shared_ptr<const Timeline> Timeline,
                              const dsl::Program &Program,
                              const std::vector<dsl::ResolvedParam> &Params,
                              const WalkForwardConfig &Config,
                              const Progress &Progress);

} // namespace core
