#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "core/dsl/ast.hpp"
#include "core/dsl/checker.hpp"
#include "core/dsl/interpreter.hpp"
#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/report/metrics.hpp" // CostBreakdown, CostFlag, Finding
#include "core/run/backtest.hpp"   // RunStatus

// Наивный против честного (ФТ-24, `decisions.md`, «Наивный и честный прогон»):
// цепочка из шести прогонов одной стратегии, каждый добавляет один фактор. Тот
// же движок (INV-3); наивный прогон - единственное исключение из INV-2.

namespace core {

// Итог прогона одного этапа: по нему вызывающий видит, что netResult неполный
// (status != Completed: банкротство или нарушение целостности E-RT01, прогон
// остановлен) и какие находки были.
struct CostStageRun {
  RunStatus Status = RunStatus::Completed;
  dsl::Level Severity = dsl::Level::Ok; // худший из находок прогона
  std::vector<Finding> Findings;
};

// Цепочка в фиксированном порядке (INV-4: без часов, потоков и случайности):
//   Naive     - naive = true, CostConfig::zero(), исполнение по close бара
//   решения; Delay     - исполнение по open следующего бара, издержки нулевые,
//   без лимита ликвидности; Fees      - + комиссия и биржевой сбор; Spread    -
//   + спред; Slippage  - + κ проскальзывания; Liquidity - + лимит ликвидности =
//   честный прогон (INV-3: результат совпадает с обычным прогоном `honest`).
// Издержки берутся из honest.costs; honest.naive = true -
// std::invalid_argument. netResult = finalEquity - initialCash, netReturn =
// netResult / initialCash. Ошибки runBacktest (std::invalid_argument) не
// перехватываются. honestSharpe - Sharpe честного прогона из отчёта (нет
// значения - флаг SharpeOver3 не ставится). afterStage - вызывается после
// каждого из шести этапов (может быть пустым): по нему вызывающий отдаёт
// прогресс и замечает закрытый поток; исключение оттуда прерывает цепочку.
using StageDone = std::function<void()>;
CostBreakdown costBreakdown(const SessionSpec &Honest,
                            std::shared_ptr<const Timeline> Timeline,
                            const dsl::Program &Program,
                            const std::vector<dsl::ResolvedParam> &Params,
                            const dsl::ParamValues &Overrides,
                            std::optional<double> HonestSharpe,
                            const StageDone &AfterStage = {});

// То же, что costBreakdown, плюс итог прогона каждого этапа (stages[i]
// соответствует breakdown.steps[i]).
struct CostBreakdownDetail {
  CostBreakdown Breakdown;
  std::vector<CostStageRun> Stages;
};
CostBreakdownDetail costBreakdownDetailed(
    const SessionSpec &Honest, std::shared_ptr<const Timeline> Timeline,
    const dsl::Program &Program, const std::vector<dsl::ResolvedParam> &Params,
    const dsl::ParamValues &Overrides, std::optional<double> HonestSharpe,
    const StageDone &AfterStage = {});

// Флаги ФТ-24 по net-результатам в рублях (R) и Sharpe честного прогона.
// Порядок в результате: CostsOverHalfProfit, SignChange, SharpeOver3.
//   CostsOverHalfProfit: R_наивн > 0 и R_наивн - R_честн > 0,5 * R_наивн;
//   SignChange:          знак R_наивн != знак R_честн (ноль - отдельный знак);
//   SharpeOver3:         honestSharpe > 3.
std::vector<CostFlag> costFlags(const Decimal &NaiveNet,
                                const Decimal &HonestNet,
                                std::optional<double> HonestSharpe);

} // namespace core
