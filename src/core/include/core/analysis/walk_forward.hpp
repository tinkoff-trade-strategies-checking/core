#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/dsl/ast.hpp"
#include "core/dsl/checker.hpp"
#include "core/dsl/interpreter.hpp"
#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "core/report/metrics.hpp"
#include "core/run/backtest.hpp"  // RunOutput, Progress

// Walk-forward (ФТ-22, `decisions.md`, «Walk-forward»): период режется на окна подбора и проверки со сдвигом в торговых
// днях шкалы; на окне подбора перебирается сетка параметров стратегии, лучший набор проверяется на следующем окне.
// Каждый прогон отдельный, с капиталом из SessionSpec. Прогоны идут по очереди, без потоков и случайности (INV-4).

namespace core {

struct WalkForwardConfig {
  std::int32_t fitDays = 250, testDays = 63, shiftDays = 63, maxGrid = 200;
};

// Номера торговых дней - в исходной шкале, с 1, границы включительно.
struct WalkForwardSegment {
  std::int32_t index;
  std::int32_t fitFromDay, fitToDay, testFromDay, testToDay;
  dsl::ParamValues chosen;  // выбранные значения всех параметров стратегии
  double fitNetReturn;      // net-доходность выбранного набора на окне подбора
  RunOutput test;           // прогон на окне проверки; шаги и сделки - в нумерации среза шкалы (дни testFromDay…testToDay)
  Report testReport;        // без бенчмарка
};

struct WalkForwardResult {
  std::vector<WalkForwardSegment> segments;
  double chainedReturn = 0;  // по цепочке проверочных окон: Π(1 + r_k) − 1
};

// Сетка: декартово произведение значений параметров в порядке объявления, последний параметр меняется быстрее всех.
// Параметр без диапазона - только значение по умолчанию; с диапазоном - от, от + шаг, … <= до. Без параметров - одна
// пустая комбинация. Размер сетки больше maxGrid или maxGrid < 1 - std::invalid_argument (до построения сетки).
std::vector<dsl::ParamValues> paramGrid(const std::vector<dsl::ResolvedParam>& params, std::int32_t maxGrid);

// Доходность по цепочке: Π(1 + r_k) − 1; без окон - 0.
double chainedReturn(const std::vector<double>& returns);

// Отрезок k: подбор [1 + shift·k; fit + shift·k], проверка - следующие testDays дней; отрезков столько, сколько
// помещается в шкалу. Критерий подбора - максимальная net-доходность ((итог − капитал) / капитал) на окне подбора, при
// равенстве - лексикографически меньший набор (порядок сетки). Прогон подбора со статусом Invalid (E-RT01) не
// засчитывается; если таких все - std::runtime_error. Уровень версии для прогонов - Ok (как в costBreakdown).
// Ошибки (std::invalid_argument): поля config <= 0, сетка больше maxGrid, ни одного отрезка, ошибка runBacktest
// (в том числе значение сетки, не прошедшее повторную проверку StrategyInstance, INV-1).
// progress - по сумме шагов всех прогонов (done монотонно растёт до total); может быть пустым.
WalkForwardResult walkForward(const SessionSpec& spec, std::shared_ptr<const Timeline> timeline,
                              const dsl::Program& program, const std::vector<dsl::ResolvedParam>& params,
                              const WalkForwardConfig& config, const Progress& progress);

}  // namespace core
