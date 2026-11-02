#include "core/engine/session_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace core {

namespace {

// Наивная сессия (ФТ-24) всегда без издержек и лимита ликвидности, что бы ни передал вызывающий.
SessionSpec normalized(SessionSpec spec) {
  if (spec.naive) spec.costs = CostConfig::zero();
  return spec;
}

bool isActive(const Order& o) { return o.status == OrderStatus::Active || o.status == OrderStatus::PartiallyFilled; }

}  // namespace

SessionEngine::SessionEngine(SessionSpec spec, std::shared_ptr<const Timeline> timeline)
    : spec_(normalized(std::move(spec))),
      timeline_(std::move(timeline)),
      exec_(spec_.instruments, spec_.costs, spec_.naive),
      portfolio_(spec_.initialCash, spec_.instruments.size()) {
  if (!timeline_) throw std::invalid_argument("SessionEngine: нет шкалы времени");
  if (timeline_->totalSteps() < 1) throw std::invalid_argument("SessionEngine: шкала времени без шагов");
  if (spec_.instruments != timeline_->instruments()) {
    throw std::invalid_argument("SessionEngine: бумаги сессии не совпадают с бумагами шкалы времени");
  }
  if (spec_.initialCash.sign() <= 0) throw std::invalid_argument("SessionEngine: начальный капитал должен быть > 0");
  recordStepState(false);
  // Единственный шаг шкалы - он же последний: сессия сразу закончена.
  if (timeline_->totalSteps() == 1) status_ = SessionStatus::FinishedEnd;
}

Validation SessionEngine::validateLegs(const std::vector<const OrderSpec*>& legs) const {
  if (status_ != SessionStatus::Running) return Validation{false, Reason::SessionNotActive, ""};
  for (const OrderSpec* leg : legs) {
    Validation v = exec_.validate(*leg, portfolio_, *timeline_, step_);
    if (!v.ok) return v;
  }
  return Validation{};
}

// Создаёт заявки команды: при отказе - со статусом Rejected и причиной (запись остаётся в журнале).
void SessionEngine::createOrders(const JournalEntry& entry, const PlaceCmd& cmd, const Validation& verdict,
                                 CommandResult& result) {
  std::vector<const OrderSpec*> legs{&cmd.order};
  if (cmd.ocoWith) legs.push_back(&*cmd.ocoWith);
  std::vector<std::int64_t> ids;
  for (std::size_t i = 0; i < legs.size(); ++i) ids.push_back(entry.seq * 2 + static_cast<std::int64_t>(i));

  for (std::size_t i = 0; i < legs.size(); ++i) {
    Order o;
    o.id = ids[i];
    o.spec = *legs[i];
    o.placedStep = step_;
    o.ruleName = entry.ruleName;
    o.because = entry.because;
    if (verdict.ok) {
      o.status = OrderStatus::Active;
      if (legs.size() == 2) o.ocoSiblingId = ids[1 - i];
    } else {
      o.status = OrderStatus::Rejected;
      o.reason = verdict.reason;
      o.invalidField = verdict.field;
      o.closedStep = step_;
    }
    orders_.push_back(std::move(o));
    changed_.push_back(ids[i]);
    result.orderIds.push_back(ids[i]);
  }
  result.accepted = verdict.ok;
  result.reason = verdict.reason;
  result.field = verdict.field;
}

Order* SessionEngine::findOrder(std::int64_t id) {
  const auto it = std::lower_bound(orders_.begin(), orders_.end(), id,
                                   [](const Order& o, std::int64_t value) { return o.id < value; });
  return it != orders_.end() && it->id == id ? &*it : nullptr;
}

void SessionEngine::closeOrder(Order& order, OrderStatus status, Reason reason) {
  order.status = status;
  order.reason = reason;
  order.closedStep = step_;
  changed_.push_back(order.id);
}

void SessionEngine::cancelActive(Reason reason) {
  for (Order& o : orders_) {
    if (isActive(o)) closeOrder(o, OrderStatus::Cancelled, reason);
  }
}

CommandResult SessionEngine::apply(const JournalEntry& entry) {
  // Остановленная сессия принимает и записи с более поздним шагом: они отклоняются с SessionNotActive,
  // чтобы журнал с такими записями оставался проигрываемым.
  const bool stopped = status_ != SessionStatus::Running;
  if (entry.step != step_ && !(stopped && entry.step > step_)) {
    throw std::invalid_argument("SessionEngine::apply: шаг записи " + std::to_string(entry.step) +
                                " не равен текущему шагу " + std::to_string(step_));
  }
  if (naiveSettled_) {
    throw std::logic_error("SessionEngine::apply: шаг уже закрыт settleStep(), записи этого шага больше не принимаются");
  }
  if (entry.seq < 0 || entry.seq <= lastSeq_) {
    throw std::invalid_argument("SessionEngine::apply: seq " + std::to_string(entry.seq) +
                                " не больше предыдущего " + std::to_string(lastSeq_));
  }
  lastSeq_ = entry.seq;

  CommandResult result;
  result.seq = entry.seq;
  const bool running = status_ == SessionStatus::Running;

  std::visit(
      [&](const auto& cmd) {
        using T = std::decay_t<decltype(cmd)>;
        if constexpr (std::is_same_v<T, PlaceCmd>) {
          std::vector<const OrderSpec*> legs{&cmd.order};
          if (cmd.ocoWith) legs.push_back(&*cmd.ocoWith);
          createOrders(entry, cmd, validateLegs(legs), result);
        } else if constexpr (std::is_same_v<T, CancelCmd>) {
          if (!running) {
            result.reason = Reason::SessionNotActive;
          } else if (cancelOrder(orders_, cmd.orderId, cmd.reason, step_, changed_)) {  // у OCO - вместе со второй ногой
            result.accepted = true;
          } else {
            result.reason = Reason::OrderNotActive;
          }
        } else {
          // Замена: сначала проверяется новая заявка; не прошла - запись отклонена целиком, старая не тронута.
          Order* old = findOrder(cmd.orderId);
          std::vector<const OrderSpec*> legs{&cmd.replacement.order};
          if (cmd.replacement.ocoWith) legs.push_back(&*cmd.replacement.ocoWith);
          Validation verdict = validateLegs(legs);
          if (verdict.ok && (old == nullptr || !isActive(*old))) verdict = Validation{false, Reason::OrderNotActive, ""};
          if (verdict.ok) {
            const Reason why = entry.ruleName.empty() ? Reason::Replaced : Reason::RuleReplaced;
            cancelOrder(orders_, old->id, why, step_, changed_);  // у OCO заменяется вся пара
          }
          createOrders(entry, cmd.replacement, verdict, result);
        }
      },
      entry.command);

  if (spec_.naive && running) naiveDue_ = true;
  return result;
}

// ФТ-24: наивное исполнение без задержки - рыночные заявки шага по close бара решения, без издержек.
void SessionEngine::settleStep() {
  if (!naiveDue_) return;
  naiveDue_ = false;
  naiveSettled_ = true;
  const std::size_t from = fills_.size();
  exec_.processNaive(step_, *timeline_, orders_, portfolio_, fills_, changed_);
  checkIntegrity(from, true);
  recordStepState(true);
}

void SessionEngine::advance() {
  if (status_ != SessionStatus::Running) throw std::logic_error("SessionEngine::advance: сессия остановлена");
  settleStep();
  const std::int64_t next = step_ + 1;
  step_ = next;
  naiveSettled_ = false;
  fillsBegin_ = fills_.size();
  changed_.clear();

  // Дневные заявки снимает processStep (задача 14), движок своего снятия не делает.
  exec_.processStep(next, *timeline_, orders_, portfolio_, fills_, changed_);
  checkIntegrity(fillsBegin_, false);
  recordStepState(false);

  const bool bankrupt = equity_.back().equity.sign() <= 0;
  const bool lastStep = next == timeline_->totalSteps() - 1;
  if (bankrupt) {
    status_ = SessionStatus::Bankrupt;
  } else if (lastStep) {
    status_ = SessionStatus::FinishedEnd;
  }
  // Остановленная сессия не оставляет активных заявок.
  if (status_ != SessionStatus::Running) cancelActive(Reason::SessionEnd);
}

// Состояние шага: переоценка по последним close, точка кривой стоимости и стоимости позиций.
void SessionEngine::recordStepState(bool replaceLast) {
  const std::size_t n = spec_.instruments.size();
  std::vector<std::optional<Decimal>> lastClose(n);
  std::vector<std::int64_t> lotSizes(n);
  std::vector<Decimal> values(n);
  for (std::size_t i = 0; i < n; ++i) {
    lotSizes[i] = spec_.instruments[i].lotSize;
    const StepBar* bar = timeline_->lastBarOf(static_cast<int>(i), step_);
    if (bar != nullptr) lastClose[i] = bar->close;
    const std::int64_t lots = portfolio_.lots(static_cast<int>(i));
    if (lots > 0 && bar != nullptr) values[i] = Portfolio::positionValue(lots, lotSizes[i], bar->close);
  }
  EquityPoint point{step_, portfolio_.equity(lastClose, lotSizes)};
  if (replaceLast) {
    equity_.back() = point;
    positionValues_.back() = std::move(values);
  } else {
    equity_.push_back(point);
    positionValues_.push_back(std::move(values));
  }
}

// INV-1, INV-2: на шаге нет баров и исполнений «из будущего», исполнение строго после шага подачи. Исключение -
// исполнения processNaive (ФТ-24: по close бара решения, sameStepAllowed); из processStep проверка действует всегда.
void SessionEngine::checkIntegrity(std::size_t fromFill, bool sameStepAllowed) const {
  for (const StepBar& bar : timeline_->barsAt(step_)) {
    if (bar.step > step_) throw IntegrityViolation("бар позже текущего шага");
  }
  for (std::size_t i = fromFill; i < fills_.size(); ++i) {
    const Fill& f = fills_[i];
    if (f.step != step_) throw IntegrityViolation("исполнение не на текущем шаге");
    const auto it = std::lower_bound(orders_.begin(), orders_.end(), f.orderId,
                                     [](const Order& o, std::int64_t value) { return o.id < value; });
    if (it == orders_.end() || it->id != f.orderId) throw IntegrityViolation("исполнение неизвестной заявки");
    if (!sameStepAllowed && f.step <= it->placedStep) {
      throw IntegrityViolation("исполнение не позже шага подачи заявки");
    }
  }
}

PortfolioSnapshot SessionEngine::snapshot() const {
  PortfolioSnapshot s;
  s.cash = portfolio_.cash();
  s.equity = equity_.back().equity;
  const std::size_t n = spec_.instruments.size();
  for (std::size_t i = 0; i < n; ++i) {
    s.lots.push_back(portfolio_.lots(static_cast<int>(i)));
    s.avgPrice.push_back(portfolio_.avgPrice(static_cast<int>(i)));
  }
  s.marketValue = positionValues_.back();
  return s;
}

StepEvents SessionEngine::lastStepEvents() const {
  StepEvents e;
  e.step = step_;
  const auto bars = timeline_->barsAt(step_);
  e.bars.assign(bars.begin(), bars.end());
  e.fills.assign(fills_.begin() + static_cast<std::ptrdiff_t>(fillsBegin_), fills_.end());
  std::vector<std::int64_t> ids = changed_;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  for (const std::int64_t id : ids) {
    const auto it = std::lower_bound(orders_.begin(), orders_.end(), id,
                                     [](const Order& o, std::int64_t value) { return o.id < value; });
    if (it != orders_.end() && it->id == id) e.changedOrders.push_back(*it);
  }
  e.portfolio = snapshot();
  return e;
}

ReplayResult replaySession(const SessionSpec& spec, std::shared_ptr<const Timeline> timeline,
                           std::span<const JournalEntry> journal, std::int64_t upToStep, std::int64_t eventsFromStep) {
  if (!timeline) throw std::invalid_argument("replaySession: нет шкалы времени");
  if (upToStep < 0 || upToStep >= timeline->totalSteps()) {
    throw std::invalid_argument("replaySession: upToStep вне шкалы времени");
  }
  // Журнал проверяется целиком до проигрывания: результат не зависит от того, где он оборвётся.
  std::int64_t prevStep = 0, prevSeq = -1;
  for (const JournalEntry& e : journal) {
    if (e.step < prevStep) throw std::invalid_argument("replaySession: шаг записи журнала убывает");
    if (e.step > upToStep) throw std::invalid_argument("replaySession: шаг записи журнала больше upToStep");
    if (e.seq < 0 || e.seq <= prevSeq) throw std::invalid_argument("replaySession: seq записей журнала не возрастает");
    prevStep = e.step;
    prevSeq = e.seq;
  }

  SessionEngine engine(spec, std::move(timeline));
  std::vector<StepEvents> events;
  std::vector<CommandResult> results;
  results.reserve(journal.size());
  std::size_t next = 0;
  for (;;) {
    const std::int64_t k = engine.currentStep();
    // Остановленная сессия принимает все оставшиеся записи (отказ SessionNotActive).
    while (next < journal.size() && (journal[next].step == k || engine.status() != SessionStatus::Running)) results.push_back(engine.apply(journal[next++]));
    engine.settleStep();
    if (k >= eventsFromStep) events.push_back(engine.lastStepEvents());
    if (k == upToStep || engine.status() != SessionStatus::Running) break;
    engine.advance();
  }

  const SessionStatus status = engine.status();
  const std::int64_t lastStep = engine.currentStep();
  return ReplayResult{status, lastStep, std::move(events), std::move(results), std::move(engine)};
}

}  // namespace core
