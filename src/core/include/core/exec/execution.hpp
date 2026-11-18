#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/exec/costs.hpp"
#include "core/exec/order.hpp"
#include "core/market/instrument.hpp"
#include "core/market/timeline.hpp"
#include "core/portfolio/portfolio.hpp"

// Единая модель исполнения для всех заявок (INV-2, INV-3): заявка,
// поданная на шаге s, исполняется не раньше open первого бара своей бумаги на
// шаге > s.

namespace core {

struct Validation {
  bool Ok = true;
  Reason ReasonCode = Reason::None;
  std::string Field; // при InvalidParams: instrument, lots, side, limitPrice,
                     // stopPrice, trailingPercent
};

// Бюджет ликвидности шага: floor(p·V) лотов на бумагу, общий для всех заявок
// шага (рыночных и условных). Один экземпляр создаётся в processStep и
// передаётся каждой стадии исполнения.
class LiquidityBudget {
public:
  explicit LiquidityBudget(std::size_t InstrumentCount)
      : Left(InstrumentCount) {}
  // Остаток бюджета бумаги по бару шага; nullopt - без лимита (liquidityShare
  // не задан). Бар без объёма при заданном лимите даёт 0.
  std::optional<std::int64_t> remaining(int Instrument, const StepBar &Bar,
                                        const CostConfig &Costs);
  // Списывает исполненные лоты; без лимита ничего не делает.
  void consume(int Instrument, std::int64_t Lots);

private:
  std::vector<std::optional<std::int64_t>> Left;
};

class ExecutionEngine {
public:
  // instruments - бумаги сессии в порядке индексов.
  ExecutionEngine(std::vector<Instrument> Instruments, CostConfig Costs);

  const CostConfig &costs() const { return Costs; }

  // Проверка заявки при подаче на шаге step: параметры, затем деньги (по close
  // последнего бара бумаги, с комиссией и сбором, без резерва под другие
  // заявки) или позиция.
  Validation validate(const OrderSpec &Spec, const Portfolio &Portfolio,
                      const Timeline &Timeline, std::int64_t Step) const;

  // Исполняет активные заявки на барах шага step; меняет orders и portfolio,
  // дописывает fills и id изменённых заявок. Порядок: рыночные -> стоп-типы
  // (стоп, стоп-лимит, трейлинг) -> лимитные и тейк-профит (в группе по id),
  // затем снятие заявок «день» (первый допустимый шаг s + 1, снимаются после
  // последнего шага его торгового дня). Бюджет ликвидности бумаги - floor(p·V)
  // лотов на все заявки шага; заявка ждёт собственного бара бумаги.
  void processStep(std::int64_t Step, const Timeline &Timeline,
                   std::vector<Order> &Orders, Portfolio &Portfolio,
                   std::vector<Fill> &Fills,
                   std::vector<std::int64_t> &Changed) const;

private:
  void processConditional(std::int64_t Step, const Timeline &Timeline,
                          LiquidityBudget &Budget, std::vector<Order> &Orders,
                          Portfolio &Portfolio, std::vector<Fill> &Fills,
                          std::vector<std::int64_t> &Changed) const;
  void processMarket(std::int64_t Step, const Timeline &Timeline,
                     LiquidityBudget &Budget, std::vector<Order> &Orders,
                     Portfolio &Portfolio, std::vector<Fill> &Fills,
                     std::vector<std::int64_t> &Changed) const;

  std::vector<Instrument> Instruments;
  CostConfig Costs;
};

// Снимает активную заявку orderId с причиной reason на шаге step; у заявки OCO
// снимается и вторая нога (с той же причиной). false - заявки нет или она не
// активна (ORDER_NOT_ACTIVE): ничего не меняется. Дописывает id снятых заявок в
// changed.
bool cancelOrder(std::vector<Order> &Orders, std::int64_t OrderId,
                 Reason ReasonCode, std::int64_t Step,
                 std::vector<std::int64_t> &Changed);

// Проверяет допустимость пары OCO: одна бумага, одна сторона, пара «стоп + тейк-профит».
// Иные пары отклоняются с причиной InvalidParams и проблемным полем ("instrument", "side", "type").
Validation validateOcoPair(const OrderSpec &First, const OrderSpec &Second);

} // namespace core
