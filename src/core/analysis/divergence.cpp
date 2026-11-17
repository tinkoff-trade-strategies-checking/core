#include "core/analysis/divergence.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

namespace core {

namespace {

// Окно «момента» W = 3 считается в шагах общей шкалы времени, а не в барах
// бумаги (при пропусках баров у бумаги окно короче). Намерения алгоритма,
// которые движок отклонил бы (нет денег или позиции), остаются его действиями:
// в копии они дают V = 0, но в сопоставлении участвуют («заявки, которые он
// создал бы»).
constexpr std::int64_t kWindow = 3;
constexpr std::size_t kMaxAlgorithms = 5; // ФТ-18
constexpr std::size_t kNone = static_cast<std::size_t>(-1);
const char *const kExcessReason = "алгоритм здесь не действовал бы";

bool isAction(const OrderSpec &spec) {
  return spec.type == OrderType::Market || spec.type == OrderType::Limit;
}

std::int64_t signedLots(const OrderSpec &spec) {
  return spec.side == Side::Buy ? spec.lots : -spec.lots;
}

int signOf(std::int64_t value) { return value > 0 ? 1 : (value < 0 ? -1 : 0); }

bool isActive(const Order &order) {
  return order.status == OrderStatus::Active ||
         order.status == OrderStatus::PartiallyFilled;
}

// Действие на шаге по бумаге: заявки одной стороны суммируются, купленное и
// проданное - со знаком.
using ActionKey = std::pair<int, std::int64_t>; // (бумага, шаг)

struct Action {
  int instrument = 0;
  std::int64_t step = 0;
  std::int64_t lots = 0; // купленное минус проданное; после finalize != 0
  std::vector<std::int64_t> orderIds; // действие человека: его принятые заявки
  std::vector<OrderSpec>
      specs; // действие алгоритма: заявки, которые он создал бы
  std::vector<std::pair<std::int64_t, std::string>>
      parts; // (лоты со знаком, because) намерений алгоритма
  std::string reason;
};

using ActionMap = std::map<ActionKey, Action>;

Action &actionAt(ActionMap &map, int instrument, std::int64_t step) {
  Action &action = map[ActionKey{instrument, step}];
  action.instrument = instrument;
  action.step = step;
  return action;
}

// Убирает шаги, где купленное и проданное сошлись в ноль (действия нет);
// причина - у первого намерения в сторону итога.
void finalize(ActionMap &map) {
  for (auto it = map.begin(); it != map.end();) {
    Action &action = it->second;
    if (action.lots == 0) {
      it = map.erase(it);
      continue;
    }
    for (const auto &[lots, because] : action.parts) {
      if (signOf(lots) == signOf(action.lots)) {
        action.reason = because;
        break;
      }
    }
    ++it;
  }
}

bool sameSpec(const OrderSpec &a, const OrderSpec &b) {
  return a.instrument == b.instrument && a.side == b.side && a.type == b.type &&
         a.lots == b.lots && a.limitPrice == b.limitPrice &&
         a.stopPrice == b.stopPrice && a.trailingPercent == b.trailingPercent &&
         a.tif == b.tif;
}

// Размеры расходятся: |q_h - q_a| > 0,2 * max(|q_h|, |q_a|) (целочисленно, без
// double).
bool sizeDiffers(std::int64_t human, std::int64_t algo) {
  const std::int64_t diff = human > algo ? human - algo : algo - human;
  const std::int64_t larger =
      std::max(human < 0 ? -human : human, algo < 0 ? -algo : algo);
  return 5 * diff > larger;
}

// Состояние сессии человека на закрытии шага k и обход шагов. onStep(k, engine)
// - до записей журнала шага k, только пока сессия идёт; onEntry(entry, result)
// - после применения каждой записи журнала. stopAt - последний шаг. Записи
// остановленной сессии принимаются и отклоняются движком (как в replaySession).
template <typename OnStep, typename OnEntry>
SessionEngine walkHuman(const SessionSpec &spec,
                        const std::shared_ptr<const Timeline> &timeline,
                        std::span<const JournalEntry> journal,
                        std::int64_t stopAt, OnStep &&onStep,
                        OnEntry &&onEntry) {
  SessionEngine engine(spec, timeline);
  std::size_t next = 0;
  for (;;) {
    const std::int64_t k = engine.currentStep();
    if (engine.status() == SessionStatus::Running)
      onStep(k, static_cast<const SessionEngine &>(engine));
    while (next < journal.size() &&
           (journal[next].step == k ||
            engine.status() != SessionStatus::Running)) {
      const CommandResult result = engine.apply(journal[next]);
      onEntry(journal[next], result);
      ++next;
    }
    engine.settleStep();
    if (k == stopAt || engine.status() != SessionStatus::Running)
      break;
    engine.advance();
  }
  return engine;
}

// Ценность действия V: сумма по исполнениям заявок ids на шагах до horizon
// включительно: знак * лоты * размер лота * (close(horizon) - цена) - комиссия
// - сбор. Без исполнений - 0.
Decimal actionValue(const std::vector<Fill> &fills,
                    const std::set<std::int64_t> &ids, std::int64_t horizon,
                    const Timeline &timeline, const SessionSpec &spec) {
  Decimal value;
  for (const Fill &fill : fills) {
    if (fill.step > horizon || ids.count(fill.orderId) == 0)
      continue;
    const StepBar *bar = timeline.lastBarOf(fill.instrument, horizon);
    if (bar == nullptr)
      continue;
    const std::int64_t units =
        fill.lots *
        spec.instruments[static_cast<std::size_t>(fill.instrument)].lotSize;
    Decimal gain = (bar->close - fill.price).times(units);
    if (fill.side == Side::Sell)
      gain = -gain;
    value += gain - fill.commission - fill.fee;
  }
  return value;
}

// Расхождение до подсчёта цены: V человека готова, V алгоритма считается копией
// движка на шаге snapStep.
struct Pending {
  Divergence divergence;
  std::size_t algorithm = 0;
  const Action *human = nullptr; // действие человека; nullptr - «упущенное»
  std::int64_t snapStep =
      -1; // шаг копии движка; -1 - у алгоритма нет действия, V алгоритма = 0
  std::int64_t percentStep =
      0; // шаг, по стоимости портфеля человека на котором считается процент
  std::vector<OrderSpec> algoSpecs; // заявки алгоритма
  Decimal humanValue, algoValue;
};

// Что стало с действием человека у одного алгоритма.
struct HumanOutcome {
  bool matched =
      false; // сопоставлено (точное совпадение, «размер» или «момент»)
  std::size_t pending = kNone; // расхождение; kNone - точное совпадение
};

Decimal medianOf(std::vector<Decimal> costs) {
  if (costs.empty())
    return Decimal();
  std::sort(costs.begin(), costs.end());
  const std::size_t n = costs.size();
  if (n % 2 == 1)
    return costs[n / 2];
  return (costs[n / 2 - 1] + costs[n / 2])
      .div(Decimal::fromUnits(2), 2, RoundingMode::HALF_UP);
}

class Analyzer {
public:
  Analyzer(const SessionSpec &spec, std::shared_ptr<const Timeline> timeline,
           std::span<const JournalEntry> journal, std::int64_t lastStep,
           const std::vector<AlgorithmInput> &algorithms)
      : spec_(spec), timeline_(std::move(timeline)), journal_(journal),
        lastStep_(lastStep), algorithms_(algorithms) {}

  DivergenceResult run() {
    validate();
    collectActions();
    for (std::size_t a = 0; a < algorithms_.size(); ++a)
      match(a);
    priceHumanSide();
    priceAlgorithmSide();
    DivergenceResult result;
    assembleDivergences(result);
    assembleMissedTrades(result);
    assembleConsensus(result);
    for (const AlgorithmInput &input : algorithms_) {
      // На том же отрезке, что и человек: по шагам 0…lastStep (сессия могла
      // закончиться раньше конца шкалы).
      result.independent.push_back(detail::runBacktestToStep(
          spec_, timeline_, input.program, input.params, input.values,
          dsl::Level::Ok, lastStep_));
    }
    return result;
  }

private:
  void validate() const {
    if (!timeline_)
      throw std::invalid_argument("analyzeDivergences: нет шкалы времени");
    if (algorithms_.size() > kMaxAlgorithms) {
      throw std::invalid_argument("analyzeDivergences: алгоритмов больше " +
                                  std::to_string(kMaxAlgorithms));
    }
    std::set<std::string> ids;
    for (const AlgorithmInput &input : algorithms_) {
      if (!ids.insert(input.id).second)
        throw std::invalid_argument(
            "analyzeDivergences: повтор id алгоритма: " + input.id);
    }
    if (spec_.naive)
      throw std::invalid_argument("analyzeDivergences: разбор только по "
                                  "честному исполнению (не naive)");
    if (lastStep_ < 0 || lastStep_ >= timeline_->totalSteps()) {
      throw std::invalid_argument(
          "analyzeDivergences: lastStep вне шкалы времени");
    }
    std::int64_t prevStep = 0, prevSeq = -1;
    for (const JournalEntry &e : journal_) {
      if (e.step < prevStep)
        throw std::invalid_argument(
            "analyzeDivergences: шаг записи журнала убывает");
      if (e.step > lastStep_)
        throw std::invalid_argument(
            "analyzeDivergences: шаг записи журнала больше lastStep");
      if (e.seq < 0 || e.seq <= prevSeq)
        throw std::invalid_argument(
            "analyzeDivergences: seq записей журнала не возрастает");
      prevStep = e.step;
      prevSeq = e.seq;
    }
  }

  // Проход 1: сессия человека по шагам; на каждом шаге каждый алгоритм видит
  // бар шага и портфель человека на закрытии.
  void collectActions() {
    std::vector<dsl::StrategyInstance> strategies;
    strategies.reserve(algorithms_.size());
    for (const AlgorithmInput &input : algorithms_) {
      strategies.emplace_back(input.program, input.params, input.values,
                              spec_.instruments.size());
      detail::warmUp(strategies.back(), *timeline_,
                     spec_); // история до шага 0 - как в независимом прогоне
    }
    algoActions_.assign(algorithms_.size(), {});
    std::vector<std::optional<std::int64_t>> barsInPosition(
        spec_.instruments.size());
    // Прошлый бар каждой бумаги и последнее намерение правила (по алгоритму):
    // повтор того же намерения на соседнем баре - та же заявка, а не новое
    // действие.
    std::vector<std::optional<std::int64_t>> previousBarStep(
        spec_.instruments.size());
    std::vector<std::int32_t> previousBarDay(spec_.instruments.size(), 0);
    struct Previous {
      OrderSpec spec;
      std::int64_t step;
    };
    std::vector<std::map<std::pair<std::string, int>, Previous>> previous(
        algorithms_.size());

    const auto onStep = [&](std::int64_t k, const SessionEngine &engine) {
      for (std::size_t i = 0; i < spec_.instruments.size(); ++i) {
        const int instrument = static_cast<int>(i);
        const StepBar *bar =
            timeline_->barOf(instrument, k); // только бар текущего шага (INV-1)
        if (bar == nullptr)
          continue;
        auto &bars = barsInPosition[i];
        if (engine.portfolio().lots(instrument) > 0) {
          bars = bars ? *bars + 1 : 0;
        } else {
          bars.reset();
        }
        dsl::StateView state;
        state.positionLots = engine.portfolio().lots(instrument);
        state.cash = engine.portfolio().cash().toDouble();
        state.equity = engine.equity().back().equity.toDouble();
        if (state.positionLots > 0) {
          state.entryPrice = engine.portfolio().avgPrice(instrument).toDouble();
          state.barsInPosition = bars;
        }
        for (std::size_t a = 0; a < strategies.size(); ++a) {
          strategies[a].onBar(instrument, *bar);
          const std::vector<dsl::Intent> intents = strategies[a].evaluate(
              instrument, *bar, spec_.instruments[i].lotSize,
              spec_.instruments[i].priceStep, state);
          for (const dsl::Intent &intent : intents) {
            if (intent.cancelAll || !isAction(intent.order))
              continue;
            // Лимитная заявка, пока не исполнилась, стоит: то же намерение того
            // же правила на следующем баре (для заявки «день» - в тот же
            // торговый день) - не новое действие. Исполнилась бы она или нет в
            // мире «как алгоритм», здесь неизвестно, поэтому повтор считается
            // продолжением. Рыночные заявки исполняются на следующем баре и
            // повторяются как новые действия.
            const auto key = std::make_pair(intent.ruleName, instrument);
            const auto found = previous[a].find(key);
            const bool repeat = found != previous[a].end() &&
                                previousBarStep[i] &&
                                found->second.step == *previousBarStep[i] &&
                                intent.order.type == OrderType::Limit &&
                                sameSpec(found->second.spec, intent.order) &&
                                (intent.order.tif == TimeInForce::Gtc ||
                                 previousBarDay[i] == bar->tradingDay);
            previous[a][key] = Previous{intent.order, k};
            if (repeat)
              continue;
            Action &action = actionAt(algoActions_[a], instrument, k);
            action.lots += signedLots(intent.order);
            action.specs.push_back(intent.order);
            action.parts.emplace_back(signedLots(intent.order), intent.because);
          }
        }
        previousBarStep[i] = k;
        previousBarDay[i] = bar->tradingDay;
      }
    };
    // Действие человека - принятая движком подача или замена рыночной либо
    // лимитной заявки. Заявка, поданная на этом же шаге и сразу заменённая или
    // снятая, действием не остаётся: считается только то, что осталось. Заявка
    // с OCO (защитная связка) в сравнении не участвует. Отклонённые движком
    // подачи действиями не считаются.
    const auto onEntry = [&](const JournalEntry &entry,
                             const CommandResult &result) {
      if (!result.accepted)
        return;
      const PlaceCmd *place = std::get_if<PlaceCmd>(&entry.command);
      std::int64_t gone = -1;
      if (const ReplaceCmd *replace = std::get_if<ReplaceCmd>(&entry.command)) {
        place = &replace->replacement;
        gone = replace->orderId;
      } else if (const CancelCmd *cancel =
                     std::get_if<CancelCmd>(&entry.command)) {
        gone = cancel->orderId;
      }
      if (gone >= 0) {
        const auto old = humanOrderToAction_.find(gone);
        if (old != humanOrderToAction_.end() &&
            old->second.second == entry.step) {
          Action &action = humanActions_.at(old->second);
          action.lots -= humanOrderLots_.at(gone);
          action.orderIds.erase(
              std::find(action.orderIds.begin(), action.orderIds.end(), gone));
          humanOrderLots_.erase(gone);
          humanOrderToAction_.erase(old);
        }
      }
      if (place == nullptr || result.orderIds.empty() || place->ocoWith ||
          !isAction(place->order))
        return;
      Action &action =
          actionAt(humanActions_, place->order.instrument, entry.step);
      action.lots += signedLots(place->order);
      action.orderIds.push_back(result.orderIds.front());
      humanOrderToAction_[result.orderIds.front()] =
          ActionKey{place->order.instrument, entry.step};
      humanOrderLots_[result.orderIds.front()] = signedLots(place->order);
    };
    human_.emplace(
        walkHuman(spec_, timeline_, journal_, lastStep_, onStep, onEntry));
    lastSession_ = human_->currentStep();
    finalize(humanActions_);
    for (ActionMap &map : algoActions_)
      finalize(map);
    // Заявки, вошедшие в действие с нулевым итогом, действием не считаются.
    for (auto it = humanOrderToAction_.begin();
         it != humanOrderToAction_.end();) {
      it = humanActions_.count(it->second) == 0 ? humanOrderToAction_.erase(it)
                                                : std::next(it);
    }
  }

  // Горизонт e: следующий шаг после t, на котором человек или этот алгоритм
  // действует по бумаге; иначе последний шаг сессии.
  std::int64_t horizonAfter(std::size_t algorithm, int instrument,
                            std::int64_t t) const {
    std::optional<std::int64_t> best;
    const auto consider = [&](const ActionMap &map) {
      const auto it = map.upper_bound(ActionKey{instrument, t});
      if (it != map.end() && it->first.first == instrument &&
          (!best || it->first.second < *best)) {
        best = it->first.second;
      }
    };
    consider(humanActions_);
    consider(algoActions_[algorithm]);
    return best ? *best : lastSession_;
  }

  // Сопоставление действий человека и алгоритма (`decisions.md`,
  // «Сопоставление»), бумага за бумагой.
  void match(std::size_t a) {
    for (std::size_t i = 0; i < spec_.instruments.size(); ++i) {
      const int instrument = static_cast<int>(i);
      std::vector<const Action *> hs, as;
      for (const auto &[key, action] : humanActions_) {
        if (key.first == instrument)
          hs.push_back(&action);
      }
      for (const auto &[key, action] : algoActions_[a]) {
        if (key.first == instrument)
          as.push_back(&action);
      }
      std::vector<char> hMatched(hs.size(), 0), aMatched(as.size(), 0);
      std::vector<std::size_t> hPending(hs.size(), kNone);

      // 1. Один шаг и один знак - совпадение; «размер», если количества
      // расходятся больше чем на 20 %.
      for (std::size_t h = 0; h < hs.size(); ++h) {
        for (std::size_t k = 0; k < as.size(); ++k) {
          if (aMatched[k] || as[k]->step != hs[h]->step ||
              signOf(as[k]->lots) != signOf(hs[h]->lots))
            continue;
          hMatched[h] = aMatched[k] = 1;
          outcome_[{a, hs[h]->instrument, hs[h]->step}].matched = true;
          if (sizeDiffers(hs[h]->lots, as[k]->lots)) {
            hPending[h] =
                makePending(a, DivergenceType::Size, hs[h], as[k], hs[h]->step);
          }
          break;
        }
      }
      // 2. «Момент»: ближайшее действие алгоритма того же знака, 1 <= |Δt| <=
      // W; при равенстве - более раннее.
      for (std::size_t h = 0; h < hs.size(); ++h) {
        if (hMatched[h])
          continue;
        std::size_t best = kNone;
        std::int64_t bestDistance = 0;
        for (std::size_t k = 0; k < as.size(); ++k) {
          if (aMatched[k] || signOf(as[k]->lots) != signOf(hs[h]->lots))
            continue;
          const std::int64_t distance = as[k]->step > hs[h]->step
                                            ? as[k]->step - hs[h]->step
                                            : hs[h]->step - as[k]->step;
          if (distance < 1 || distance > kWindow)
            continue;
          if (best == kNone || distance < bestDistance) {
            best = k;
            bestDistance = distance;
          }
        }
        if (best == kNone)
          continue;
        hMatched[h] = aMatched[best] = 1;
        outcome_[{a, hs[h]->instrument, hs[h]->step}].matched = true;
        hPending[h] = makePending(a, DivergenceType::Timing, hs[h], as[best],
                                  std::max(hs[h]->step, as[best]->step));
      }
      // 3. Остальное: противоположные действия одного шага - одно «лишнее»;
      // прочие - «лишнее» и «упущенное».
      for (std::size_t h = 0; h < hs.size(); ++h) {
        if (hMatched[h])
          continue;
        const Action *opposite = nullptr;
        for (std::size_t k = 0; k < as.size(); ++k) {
          if (!aMatched[k] && as[k]->step == hs[h]->step) {
            aMatched[k] = 1;
            opposite = as[k];
            break;
          }
        }
        hPending[h] = makePending(a, DivergenceType::Excess, hs[h], opposite,
                                  hs[h]->step);
      }
      for (std::size_t k = 0; k < as.size(); ++k) {
        if (!aMatched[k])
          makePending(a, DivergenceType::Missed, nullptr, as[k], as[k]->step);
      }
      for (std::size_t h = 0; h < hs.size(); ++h)
        outcome_[{a, hs[h]->instrument, hs[h]->step}].pending = hPending[h];
    }
  }

  std::size_t makePending(std::size_t a, DivergenceType type,
                          const Action *human, const Action *algo,
                          std::int64_t at) {
    Pending p;
    p.algorithm = a;
    p.human = human;
    Divergence &d = p.divergence;
    d.algorithmId = algorithms_[a].id;
    d.instrument = human != nullptr ? human->instrument : algo->instrument;
    d.type = type;
    if (human != nullptr) {
      d.humanStep = human->step;
      d.humanLots = human->lots;
    }
    if (algo != nullptr) {
      d.algoStep = algo->step;
      d.algoLots = algo->lots;
      p.algoSpecs = algo->specs;
      p.snapStep =
          algo->step; // копия движка человека на шаге действия алгоритма
    }
    // Горизонт и процент - от шага расхождения: у «момента» это более поздний
    // из двух шагов.
    d.horizonStep = horizonAfter(a, d.instrument, at);
    p.percentStep = at;
    d.reason = type == DivergenceType::Excess
                   ? kExcessReason
                   : (algo != nullptr ? algo->reason : "");
    pending_.push_back(std::move(p));
    return pending_.size() - 1;
  }

  // V человека: фактические исполнения его заявок действия на шагах до
  // горизонта.
  void priceHumanSide() {
    for (Pending &p : pending_) {
      if (p.human == nullptr)
        continue;
      const std::set<std::int64_t> ids(p.human->orderIds.begin(),
                                       p.human->orderIds.end());
      p.humanValue = actionValue(human_->fills(), ids, p.divergence.horizonStep,
                                 *timeline_, spec_);
    }
  }

  // Проход 2: V алгоритма - копия сессии человека на шаге действия алгоритма,
  // активные заявки человека сняты, заявки алгоритма применены, затем advance()
  // до горизонта (INV-3: тот же движок).
  void priceAlgorithmSide() {
    std::map<std::int64_t, std::vector<std::size_t>> jobs;
    for (std::size_t index = 0; index < pending_.size(); ++index) {
      if (pending_[index].snapStep >= 0)
        jobs[pending_[index].snapStep].push_back(index);
    }
    if (!jobs.empty()) {
      const std::int64_t seqBase =
          (journal_.empty() ? 0 : journal_.back().seq) + 1;
      const auto onStep = [&](std::int64_t k, const SessionEngine &engine) {
        const auto found = jobs.find(k);
        if (found == jobs.end())
          return;
        for (const std::size_t index : found->second)
          pending_[index].algoValue =
              algorithmValue(engine, pending_[index], seqBase);
      };
      walkHuman(spec_, timeline_, journal_, jobs.rbegin()->first, onStep,
                [](const JournalEntry &, const CommandResult &) {});
    }
    for (Pending &p : pending_) {
      Divergence &d = p.divergence;
      d.cost = (p.algoValue - p.humanValue).round(2, RoundingMode::HALF_UP);
      const auto &equity = human_->equity();
      const std::size_t at = static_cast<std::size_t>(p.percentStep);
      const double total =
          at < equity.size() ? equity[at].equity.toDouble() : 0;
      d.costPercent = total > 0 ? d.cost.toDouble() / total * 100.0 : 0.0;
    }
  }

  Decimal algorithmValue(const SessionEngine &at, const Pending &p,
                         std::int64_t seqBase) const {
    SessionEngine copy = at;
    std::int64_t seq = seqBase;
    const std::int64_t step = copy.currentStep();
    for (const Order &order : at.orders()) {
      if (!isActive(order))
        continue;
      JournalEntry cancel;
      cancel.seq = seq++;
      cancel.step = step;
      cancel.command = CancelCmd{order.id, Reason::CancelledByUser};
      copy.apply(cancel);
    }
    std::set<std::int64_t> ids;
    for (const OrderSpec &spec : p.algoSpecs) {
      JournalEntry entry;
      entry.seq = seq++;
      entry.step = step;
      entry.command = PlaceCmd{spec, std::nullopt};
      const CommandResult result = copy.apply(entry);
      ids.insert(result.orderIds.begin(), result.orderIds.end());
    }
    while (copy.currentStep() < p.divergence.horizonStep &&
           copy.status() == SessionStatus::Running)
      copy.advance();
    return actionValue(copy.fills(), ids, p.divergence.horizonStep, *timeline_,
                       spec_);
  }

  void assembleDivergences(DivergenceResult &result) const {
    for (std::size_t a = 0; a < algorithms_.size(); ++a) {
      std::vector<const Divergence *> mine;
      for (const Pending &p : pending_) {
        if (p.algorithm == a)
          mine.push_back(&p.divergence);
      }
      const auto primary = [](const Divergence &d) {
        return d.humanStep >= 0 ? d.humanStep : d.algoStep;
      };
      std::stable_sort(mine.begin(), mine.end(),
                       [&](const Divergence *x, const Divergence *y) {
                         return std::make_tuple(primary(*x), x->instrument,
                                                static_cast<int>(x->type)) <
                                std::make_tuple(primary(*y), y->instrument,
                                                static_cast<int>(y->type));
                       });
      for (const Divergence *d : mine)
        result.divergences.push_back(*d);
    }
  }

  // ФТ-20: закрытая сделка человека с прибылью после издержек, вход которой -
  // «лишнее действие» алгоритма.
  void assembleMissedTrades(DivergenceResult &result) const {
    for (const RoundTrip &trip : human_->portfolio().roundTrips()) {
      if (trip.netPnl.sign() <= 0)
        continue;
      const auto entry = humanOrderToAction_.find(trip.entryOrderId);
      if (entry == humanOrderToAction_.end())
        continue;
      MissedProfitableTrade missed;
      missed.instrument = trip.instrument;
      missed.entryStep = trip.entryStep;
      missed.exitStep = trip.exitStep;
      missed.lots = trip.lots;
      missed.netPnl = trip.netPnl;
      const std::int64_t lotSize =
          spec_.instruments.at(static_cast<std::size_t>(trip.instrument))
              .lotSize;
      const Decimal units = Decimal::fromUnits(trip.lots * lotSize);
      missed.entryPrice = trip.entryValue.div(units, RoundingMode::HALF_UP);
      missed.exitPrice = trip.exitValue.div(units, RoundingMode::HALF_UP);
      for (std::size_t a = 0; a < algorithms_.size(); ++a) {
        const auto found =
            outcome_.find({a, entry->second.first, entry->second.second});
        if (found != outcome_.end() && !found->second.matched)
          missed.algorithmIds.push_back(algorithms_[a].id);
      }
      if (!missed.algorithmIds.empty())
        result.missed.push_back(std::move(missed));
    }
  }

  void assembleConsensus(DivergenceResult &result) const {
    const std::int32_t total = static_cast<std::int32_t>(algorithms_.size());
    if (total == 0)
      return;
    std::vector<ConsensusItem> human;
    for (const auto &[key, action] : humanActions_) {
      std::int32_t agreeing = 0;
      std::vector<Decimal> costs;
      for (std::size_t a = 0; a < algorithms_.size(); ++a) {
        const auto found = outcome_.find({a, key.first, key.second});
        if (found == outcome_.end())
          continue;
        if (found->second.matched) {
          ++agreeing;
        } else if (found->second.pending != kNone) {
          // Медиана - по тем, кто «здесь не действовал бы» (смысл сообщения):
          // «размер» и «момент» в неё не входят.
          costs.push_back(pending_[found->second.pending].divergence.cost);
        }
      }
      if (agreeing == total)
        continue;
      human.push_back(ConsensusItem{key.first, key.second,
                                    action.lots > 0 ? Side::Buy : Side::Sell,
                                    true, agreeing, total, medianOf(costs)});
    }
    std::sort(human.begin(), human.end(),
              [](const ConsensusItem &x, const ConsensusItem &y) {
                return std::make_tuple(x.step, x.instrument) <
                       std::make_tuple(y.step, y.instrument);
              });

    // Упущенные действия: (бумага, шаг, сторона) -> цены ошибки алгоритмов,
    // которые действовали бы здесь.
    std::map<std::tuple<std::int64_t, int, int>, std::vector<Decimal>> missed;
    for (const Pending &p : pending_) {
      const Divergence &d = p.divergence;
      if (d.type != DivergenceType::Missed)
        continue;
      missed[{d.algoStep, d.instrument, d.algoLots > 0 ? 0 : 1}].push_back(
          d.cost);
    }
    for (auto &item : human)
      result.consensus.push_back(std::move(item));
    for (const auto &[key, costs] : missed) {
      const auto &[step, instrument, sideIndex] = key;
      result.consensus.push_back(ConsensusItem{
          instrument, step, sideIndex == 0 ? Side::Buy : Side::Sell, false,
          static_cast<std::int32_t>(costs.size()), total, medianOf(costs)});
    }
  }

  const SessionSpec &spec_;
  std::shared_ptr<const Timeline> timeline_;
  std::span<const JournalEntry> journal_;
  std::int64_t lastStep_;
  const std::vector<AlgorithmInput> &algorithms_;

  ActionMap humanActions_;
  std::vector<ActionMap> algoActions_;
  std::map<std::int64_t, ActionKey>
      humanOrderToAction_; // id заявки человека -> его действие
  std::map<std::int64_t, std::int64_t>
      humanOrderLots_; // id заявки человека -> лоты со знаком
  std::optional<SessionEngine> human_; // сессия человека на последнем шаге
  std::int64_t lastSession_ = 0;
  std::vector<Pending> pending_;
  std::map<std::tuple<std::size_t, int, std::int64_t>, HumanOutcome>
      outcome_; // (алгоритм, бумага, шаг человека)
};

} // namespace

DivergenceResult
analyzeDivergences(const SessionSpec &spec,
                   std::shared_ptr<const Timeline> timeline,
                   std::span<const JournalEntry> human, std::int64_t lastStep,
                   const std::vector<AlgorithmInput> &algorithms) {
  return Analyzer(spec, std::move(timeline), human, lastStep, algorithms).run();
}

} // namespace core
