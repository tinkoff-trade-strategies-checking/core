#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include "core/engine/journal.hpp"
#include "core/exec/costs.hpp"
#include "core/exec/execution.hpp"
#include "core/exec/order.hpp"
#include "core/market/instrument.hpp"
#include "core/market/step_bar.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp"

// Движок сессии: проигрывание журнала по шагам общей шкалы времени (ФТ-6, ФТ-10, INV-1, INV-2, INV-8).
// Одни и те же вход и журнал дают одно и то же состояние. Наружу движок отдаёт только то, что произошло
// на шагах 0…currentStep(): Timeline целиком остаётся внутри (INV-1).

namespace core {

struct PortfolioSnapshot {
  Decimal cash, equity;
  std::vector<std::int64_t> lots;           // по бумагам
  std::vector<Decimal> avgPrice;            // средневзвешенная цена покупки, 0 без позиции
  std::vector<Decimal> marketValue;         // стоимость позиции по close последнего бара, 0 без позиции
};

struct StepEvents {
  std::int64_t step = 0;
  std::vector<StepBar> bars;           // бары шага
  std::vector<Fill> fills;             // исполнения шага
  std::vector<Order> changedOrders;    // заявки, изменившиеся на шаге (подача, исполнение, снятие), по возрастанию id
  PortfolioSnapshot portfolio;         // портфель после всех событий шага
};

enum class SessionStatus { Running, FinishedEnd, Bankrupt };

struct SessionSpec {
  std::vector<Instrument> instruments;
  CostConfig costs;
  Decimal initialCash;
  // ФТ-24: исполнение по close бара решения (единственное исключение из INV-2). Движок сам берёт CostConfig::zero():
  // рыночные заявки идут через processNaive, остальные (processStep) исполняются без издержек.
  bool naive = false;
};

// Нарушена целостность времени: исполнение не на текущем шаге, неизвестной заявки или не позже шага подачи (INV-2).
struct IntegrityViolation : std::logic_error {
  using std::logic_error::logic_error;
};

// Копируемый: копия - снимок состояния (нужен «что если», задача 29); шкала времени общая и неизменяемая.
class SessionEngine {
 public:
  // initialCash <= 0, пустая шкала или несовпадение бумаг со шкалой - std::invalid_argument.
  SessionEngine(SessionSpec spec, std::shared_ptr<const Timeline> timeline);

  // Применяет запись на текущем шаге (действует с шага currentStep() + 1). Шаг записи не равен currentStep() или seq не
  // больше предыдущего - std::invalid_argument. После остановки сессии принимаются записи и с более поздним шагом:
  // они отклоняются с SessionNotActive. Запись на шаге, уже закрытом settleStep() (наивный режим), - std::logic_error.
  CommandResult apply(const JournalEntry& entry);
  // Наивный режим (ФТ-24): исполняет рыночные заявки шага по close бара решения - после всех записей шага, поэтому
  // проверка каждой записи видит портфель до исполнений шага. Идемпотентна; advance() вызывает её сам, а чтение
  // состояния шага после последней записи (portfolio(), fills(), ...) требует вызова вручную. Без naive - ничего не делает.
  void settleStep();
  // Обрабатывает шаг currentStep() + 1 (сначала settleStep()). Остановленная сессия - std::logic_error.
  void advance();

  std::int64_t currentStep() const { return step_; }
  SessionStatus status() const { return status_; }
  const Portfolio& portfolio() const { return portfolio_; }
  const std::vector<Order>& orders() const { return orders_; }
  const std::vector<Fill>& fills() const { return fills_; }
  // По точке на каждый пройденный шаг, с 0: equity().size() == currentStep() + 1.
  const std::vector<EquityPoint>& equity() const { return equity_; }
  // [шаг][бумага]: стоимость позиции по close шага (для HHI и крупнейшей позиции).
  const std::vector<std::vector<Decimal>>& positionValues() const { return positionValues_; }
  StepEvents lastStepEvents() const;
  PortfolioSnapshot snapshot() const;

 private:
  Validation validateLegs(const std::vector<const OrderSpec*>& legs) const;
  void createOrders(const JournalEntry& entry, const PlaceCmd& cmd, const Validation& verdict, CommandResult& result);
  Order* findOrder(std::int64_t id);
  void closeOrder(Order& order, OrderStatus status, Reason reason);
  void cancelActive(Reason reason);
  void recordStepState(bool replaceLast);
  void checkIntegrity(std::size_t fromFill, bool sameStepAllowed) const;

  SessionSpec spec_;
  std::shared_ptr<const Timeline> timeline_;
  ExecutionEngine exec_;
  Portfolio portfolio_;
  std::vector<Order> orders_;  // по возрастанию id
  std::vector<Fill> fills_;
  std::vector<EquityPoint> equity_;
  std::vector<std::vector<Decimal>> positionValues_;
  std::vector<std::int64_t> changed_;  // id заявок, изменившихся на текущем шаге
  std::size_t fillsBegin_ = 0;         // начало исполнений текущего шага в fills_
  std::int64_t step_ = 0;
  std::int64_t lastSeq_ = -1;
  bool naiveSettled_ = false;          // settleStep() исполнил наивные заявки шага: записи шага больше не принимаются
  bool naiveDue_ = false;              // после записей шага ещё не вызван processNaive
  SessionStatus status_ = SessionStatus::Running;
};

struct ReplayResult {
  SessionStatus status;
  std::int64_t lastStep;
  std::vector<StepEvents> events;              // шаги eventsFromStep…lastStep
  std::vector<CommandResult> commandResults;   // по записи журнала, в порядке журнала
  SessionEngine engine;                        // состояние на lastStep: можно продолжить или скопировать
};

// Проигрывает журнал с шага 0 до upToStep (или до остановки сессии раньше). Записи с убывающим step (равные - можно)
// или seq, с step < 0 или step > upToStep, upToStep вне шкалы - std::invalid_argument. Записи после остановки сессии
// (конец периода, банкротство) не ошибка: они отклоняются с SessionNotActive.
// Бары позже upToStep нигде в результате не появляются (INV-1).
ReplayResult replaySession(const SessionSpec& spec, std::shared_ptr<const Timeline> timeline,
                           std::span<const JournalEntry> journal, std::int64_t upToStep, std::int64_t eventsFromStep);

}  // namespace core
