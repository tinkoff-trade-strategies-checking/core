#pragma once

#include <cstdint>

#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"

namespace core {

// Параметры стресс-сценария (ФТ-23, `decisions.md`, «Стресс-сценарий»).
// Применяется к данным до того, как их видит стратегия. Для каждой бумаги
// событие начинается с её первого бара на шаге
// >= startStep (t0), окно волатильности и объёма - её volBars баров с t0
// (меньше, если баров не хватает).
struct StressScenario {
  std::int64_t startStep = 0; // шаг начала события, >= 0
  Decimal gapPercent; // X, %: гэп -X % на все цены с t0 до конца; 0 <= X < 100
  Decimal volMultiplier; // k: волатильность x k на окне; k >= 0
  std::int32_t volBars =
      0; // N: длина окна в барах, >= 0 (0 - без окна волатильности и объёма)
  Decimal volumeShare; // q: объём окна до floor(V * q) лотов; 0 <= q <= 1
};

// Копия шкалы со стрессом: гэп, затем волатильность, затем объём. Все операции
// Decimal - 9 знаков HALF_UP; цены бара округляются до шага цены HALF_UP сразу,
// цена <= 0 равна шагу цены; рекуррентность волатильности берёт уже округлённый
// c'. После окна цены умножаются на f = c'/c последнего бара окна (c - закрытие
// после гэпа). Для бумаги без бара до t0 опорное закрытие - open её первого
// бара t0. Исходная шкала не меняется. Неверные параметры -
// std::invalid_argument.
Timeline applyStress(const Timeline &timeline, const StressScenario &scenario);

} // namespace core
