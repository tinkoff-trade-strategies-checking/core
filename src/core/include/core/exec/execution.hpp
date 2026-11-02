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

// Единая модель исполнения для человека и алгоритма (INV-2, INV-3): заявка, поданная на шаге s,
// исполняется не раньше open первого бара своей бумаги на шаге > s.

namespace core {

struct Validation {
  bool ok = true;
  Reason reason = Reason::None;
  std::string field;  // при InvalidParams: instrument, lots, side, limitPrice, stopPrice, trailingPercent
};

// Бюджет ликвидности шага: floor(p·V) лотов на бумагу, общий для всех заявок шага (рыночных и условных).
// Один экземпляр создаётся в processStep и передаётся каждой стадии исполнения.
class LiquidityBudget {
 public:
  explicit LiquidityBudget(std::size_t instrumentCount) : left_(instrumentCount) {}
  // Остаток бюджета бумаги по бару шага; nullopt - без лимита (liquidityShare не задан).
  // Бар без объёма при заданном лимите даёт 0.
  std::optional<std::int64_t> remaining(int instrument, const StepBar& bar, const CostConfig& costs);
  // Списывает исполненные лоты; без лимита ничего не делает.
  void consume(int instrument, std::int64_t lots);

 private:
  std::vector<std::optional<std::int64_t>> left_;
};

class ExecutionEngine {
 public:
  // instruments - бумаги сессии в порядке индексов; naive - признак режима ФТ-24 (вызывающий выбирает
  // между processStep и processNaive сам).
  ExecutionEngine(std::vector<Instrument> instruments, CostConfig costs, bool naive);

  bool naive() const { return naive_; }
  const CostConfig& costs() const { return costs_; }

  // Проверка заявки при подаче на шаге step: параметры, затем деньги (по close последнего бара бумаги,
  // с комиссией и сбором, без резерва под другие заявки) или позиция.
  Validation validate(const OrderSpec& spec, const Portfolio& portfolio, const Timeline& timeline,
                      std::int64_t step) const;

  // Исполняет активные заявки на барах шага step; меняет orders и portfolio, дописывает fills и id
  // изменённых заявок. Порядок: рыночные -> стоп-типы (стоп, стоп-лимит, трейлинг) -> лимитные и тейк-профит
  // (в группе по id), затем снятие заявок «день» (первый допустимый шаг s + 1, снимаются после последнего шага
  // его торгового дня). Бюджет ликвидности бумаги - floor(p·V) лотов на все заявки шага; заявка ждёт
  // собственного бара бумаги.
  void processStep(std::int64_t step, const Timeline& timeline, std::vector<Order>& orders, Portfolio& portfolio,
                   std::vector<Fill>& fills, std::vector<std::int64_t>& changed) const;

  // ФТ-24: только для движка с naive = true, иначе std::logic_error (исключение из INV-2 доступно лишь за флагом).
  // Рыночные заявки (поданные на шагах <= step) по close последнего бара бумаги не позже step;
  // без издержек, спреда, проскальзывания и лимита ликвидности. Деньги и позиция ограничивают объём.
  void processNaive(std::int64_t step, const Timeline& timeline, std::vector<Order>& orders, Portfolio& portfolio,
                    std::vector<Fill>& fills, std::vector<std::int64_t>& changed) const;

 private:
  void processConditional(std::int64_t step, const Timeline& timeline, LiquidityBudget& budget,
                          std::vector<Order>& orders, Portfolio& portfolio, std::vector<Fill>& fills,
                          std::vector<std::int64_t>& changed) const;
  void processMarket(std::int64_t step, const Timeline& timeline, LiquidityBudget& budget, std::vector<Order>& orders,
                     Portfolio& portfolio, std::vector<Fill>& fills, std::vector<std::int64_t>& changed) const;

  std::vector<Instrument> instruments_;
  CostConfig costs_;
  bool naive_;
};

// Снимает активную заявку orderId с причиной reason на шаге step; у заявки OCO снимается и вторая нога
// (с той же причиной). false - заявки нет или она не активна (ORDER_NOT_ACTIVE): ничего не меняется.
// Дописывает id снятых заявок в changed.
bool cancelOrder(std::vector<Order>& orders, std::int64_t orderId, Reason reason, std::int64_t step,
                 std::vector<std::int64_t>& changed);

}  // namespace core
