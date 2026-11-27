#pragma once

#include <cstdint>
#include <memory>
#include <optional>
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

// Движок сессии: проигрывание журнала по шагам общей шкалы времени (ФТ-6,
// ФТ-10, INV-1, INV-2, INV-8). Одни и те же вход и журнал дают одно и то же
// состояние. Наружу движок отдаёт только то, что произошло на шагах
// 0…currentStep(): Timeline целиком остаётся внутри (INV-1).

namespace core {

struct PortfolioSnapshot {
  Decimal Cash, Equity;
  std::vector<std::int64_t> Lots; // по бумагам
  std::vector<Decimal> AvgPrice; // средневзвешенная цена покупки, 0 без позиции
  std::vector<Decimal>
      MarketValue; // стоимость позиции по close последнего бара, 0 без позиции
  std::vector<Decimal> CostBasis; // себестоимость открытой позиции (core#54)
  std::vector<std::optional<std::int64_t>> BarsInPosition;
  std::vector<bool> PositionReopenedOnBar;
};

struct StepEvents {
  std::int64_t Step = 0;
  std::vector<StepBar> Bars; // бары шага
  std::vector<Fill> Fills;   // исполнения шага
  std::vector<Order> ChangedOrders; // заявки, изменившиеся на шаге (подача,
                                    // исполнение, снятие), по возрастанию id
  PortfolioSnapshot Portfolio; // портфель после всех событий шага
};

enum class SessionStatus { Running, FinishedEnd, Bankrupt };

struct SessionSpec {
  std::vector<Instrument> Instruments;
  CostConfig Costs;
  Decimal InitialCash;
  MarginConfig Margin{};
};

// Нарушена целостность времени: исполнение не на текущем шаге, неизвестной
// заявки или не позже шага подачи (INV-2).
struct IntegrityViolation : std::logic_error {
  using std::logic_error::logic_error;
};

// Копируемый: копия - снимок состояния (нужен «что если», задача 29); шкала
// времени общая и неизменяемая.
class SessionEngine {
public:
  // initialCash <= 0, пустая шкала или несовпадение бумаг со шкалой -
  // std::invalid_argument.
  SessionEngine(SessionSpec Spec, std::shared_ptr<const Timeline> Timeline);

  // Применяет запись на текущем шаге (действует с шага currentStep() + 1). Шаг
  // записи не равен currentStep() или seq не больше предыдущего -
  // std::invalid_argument. После остановки сессии принимаются записи и с более
  // поздним шагом: они отклоняются с SessionNotActive.
  CommandResult apply(const JournalEntry &Entry);
  // Обрабатывает шаг currentStep() + 1. Остановленная
  // сессия - std::logic_error.
  void advance();

  std::int64_t currentStep() const { return Step; }
  SessionStatus status() const { return Status; }
  const Portfolio &portfolio() const { return PortfolioState; }
  const std::vector<Order> &orders() const { return Orders; }
  const std::vector<Fill> &fills() const { return Fills; }
  const Decimal &overnightFees() const { return OvernightFees; }
  std::int64_t marginCallCount() const { return MarginCalls; }
  std::int64_t liquidationCount() const { return Liquidations; }
  // По точке на каждый пройденный шаг, с 0: equity().size() == currentStep()
  // + 1.
  const std::vector<EquityPoint> &equity() const { return Equity; }
  // [шаг][бумага]: стоимость позиции по close шага (для HHI и крупнейшей
  // позиции).
  const std::vector<std::vector<Decimal>> &positionValues() const {
    return PositionValues;
  }
  StepEvents lastStepEvents() const;
  PortfolioSnapshot snapshot() const;

private:
  Validation validateLegs(const std::vector<const OrderSpec *> &Legs) const;
  void createOrders(const JournalEntry &Entry, const PlaceCmd &Cmd,
                    const Validation &Verdict, CommandResult &Result);
  Order *findOrder(std::int64_t Id);
  void closeOrder(Order &Order, OrderStatus Status, Reason Reason);
  void cancelActive(Reason Reason);
  void chargeOvernightCarry(std::int64_t PrevStep, std::int64_t NextStep);
  void checkMaintenanceMargin();
  void updatePositionBars(const std::vector<std::int64_t> &PosBefore);
  void recordStepState();
  void checkIntegrity(std::size_t FromFill) const;

  SessionSpec Spec;
  std::shared_ptr<const Timeline> TimelineRef;
  ExecutionEngine Exec;
  Portfolio PortfolioState;
  std::vector<Order> Orders; // по возрастанию id
  std::vector<Fill> Fills;
  std::vector<EquityPoint> Equity;
  std::vector<std::vector<Decimal>> PositionValues;
  std::vector<std::int64_t> Changed; // id заявок, изменившихся на текущем шаге
  std::vector<std::optional<std::int64_t>> BarsInPosition;
  std::vector<bool> PositionReopenedOnBar;
  Decimal OvernightFees;
  std::int64_t MarginCalls = 0;
  std::int64_t Liquidations = 0;
  std::size_t FillsBegin = 0; // начало исполнений текущего шага в fills_
  std::int64_t Step = 0;
  std::int64_t LastSeq = -1;
  SessionStatus Status = SessionStatus::Running;
};

struct ReplayResult {
  SessionStatus Status;
  std::int64_t LastStep;
  std::vector<StepEvents> Events; // шаги eventsFromStep…lastStep
  std::vector<CommandResult>
      CommandResults; // по записи журнала, в порядке журнала
  SessionEngine
      Engine; // состояние на lastStep: можно продолжить или скопировать
};

// Проигрывает журнал с шага 0 до upToStep (или до остановки сессии раньше).
// Записи с убывающим step (равные - можно) или seq, с step < 0 или step >
// upToStep, upToStep вне шкалы - std::invalid_argument. Записи после остановки
// сессии (конец периода, банкротство) не ошибка: они отклоняются с
// SessionNotActive. Бары позже upToStep нигде в результате не появляются
// (INV-1).
ReplayResult replaySession(const SessionSpec &Spec,
                           std::shared_ptr<const Timeline> Timeline,
                           std::span<const JournalEntry> Journal,
                           std::int64_t UpToStep, std::int64_t EventsFromStep);

struct OrderEstimate {
  Validation Check;
  Decimal ReferencePrice;
  Decimal Value;
  Decimal Commission;
  Decimal Fee;
  Decimal RequiredCash;
  Decimal ValuePct;
  Decimal CommissionPct;
  Decimal FeePct;
  Decimal RequiredCashPct;
  // Заполняется, если входная заявка имела IsBlindPrice = true и перевод в
  // рубли прошёл успешно (содержит рублёвые цены и IsBlindPrice = false).
  std::optional<OrderSpec> ResolvedOrder;
};

// Оценка заявки на шаге UpToStep по close последнего бара (шаг <= курсора)
// с проверкой денег/позиции как при подаче (`decisions.md`, «Заявки»).
// Журнал не меняется (INV-8), бары позже UpToStep не используются (INV-1).
OrderEstimate estimateOrder(const SessionSpec &Spec,
                            std::shared_ptr<const Timeline> Timeline,
                            std::span<const JournalEntry> Journal,
                            std::int64_t UpToStep, const OrderSpec &Order);

} // namespace core
