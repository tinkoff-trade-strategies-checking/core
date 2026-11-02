#include "core/run/backtest.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace core {

namespace {

bool isActive(const Order& o) { return o.status == OrderStatus::Active || o.status == OrderStatus::PartiallyFilled; }

bool sameSpec(const OrderSpec& a, const OrderSpec& b) {
  return a.instrument == b.instrument && a.side == b.side && a.type == b.type && a.lots == b.lots &&
         a.limitPrice == b.limitPrice && a.stopPrice == b.stopPrice && a.trailingPercent == b.trailingPercent &&
         a.tif == b.tif;
}

// Прогон одной стратегии: состояние цикла собрано в класс, чтобы цикл читался сверху вниз.
class Runner {
 public:
  Runner(const SessionSpec& spec, std::shared_ptr<const Timeline> timeline, const dsl::Program& program,
         const std::vector<dsl::ResolvedParam>& params, const dsl::ParamValues& overrides, const Progress& progress,
         const detail::StepHook& hook, std::int64_t lastStep)
      : spec_(spec),
        timeline_(timeline),
        engine_(spec, std::move(timeline)),
        strategy_(program, params, overrides, spec.instruments.size()),
        progress_(progress),
        hook_(hook),
        lastStep_(lastStep),
        barsInPosition_(spec.instruments.size()) {}

  RunOutput run(dsl::Level staticLevel) {
    RunOutput out;
    std::optional<Finding> violation;
    try {
      loop();
    } catch (const IntegrityViolation& e) {
      violation = detail::integrityFinding(e, engine_.currentStep());
      // Последний отсчёт прогресса уходит и при остановке: done - число шагов с точкой кривой стоимости.
      reportProgress(engine_.equity().back().step + 1);
    }
    collect(out, violation.has_value());
    if (violation) {
      out.findings.push_back(*violation);
    } else {
      out.findings = detail::runtimeFindings(*timeline_, out.orders, out.fills, out.roundTrips);
    }
    out.level = detail::worstLevel(staticLevel, out.findings);
    out.digest = digestOf(out);
    return out;
  }

 private:
  static constexpr std::int64_t kProgressEvery = 1000;

  void loop() {
    for (;;) {
      const std::int64_t k = engine_.currentStep();
      if (hook_) hook_(engine_);
      if (engine_.status() == SessionStatus::Running) {
        cancelFlatSells(k);
        decide(k);
      }
      // Прогон до lastStep: на нём стратегия ещё решает (заявка не исполнится), дальше шаги не обрабатываются.
      const bool stop = engine_.status() != SessionStatus::Running || k >= lastStep_;
      if (!stop && (k + 1) % kProgressEvery == 0) reportProgress(k + 1);
      if (stop) break;
      engine_.advance();
    }
    reportProgress(engine_.currentStep() + 1);
  }

  // Отсчёт прогресса без повтора одного и того же значения.
  void reportProgress(std::int64_t done) {
    if (!progress_ || done == reportedDone_) return;
    reportedDone_ = done;
    progress_(done, timeline_->totalSteps());
  }

  const Order* order(std::int64_t id) const {
    const auto& orders = engine_.orders();
    const auto it = std::lower_bound(orders.begin(), orders.end(), id,
                                     [](const Order& o, std::int64_t value) { return o.id < value; });
    return it != orders.end() && it->id == id ? &*it : nullptr;
  }

  // Запись журнала стратегии и её применение тем же путём, что у журнала человека (INV-3).
  template <typename Command>
  CommandResult apply(std::int64_t step, Command command, const std::string& rule, const std::string& because) {
    JournalEntry entry;
    entry.seq = seq_++;
    entry.step = step;
    entry.command = std::move(command);
    entry.ruleName = rule;
    entry.because = because;
    journal_.push_back(entry);
    return engine_.apply(entry);
  }

  // После шага: по бумагам с нулевой позицией активные продажи стратегии снимаются.
  void cancelFlatSells(std::int64_t k) {
    for (auto it = book_.begin(); it != book_.end();) {
      const Order* o = order(it->second);
      if (o == nullptr || !isActive(*o)) {
        it = book_.erase(it);
      } else if (o->spec.side == Side::Sell && engine_.portfolio().lots(o->spec.instrument) == 0) {
        const std::string rule = o->ruleName;
        apply(k, CancelCmd{o->id, Reason::PositionClosed}, rule, "позиция закрыта");
        it = book_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void decide(std::int64_t k) {
    for (std::size_t i = 0; i < spec_.instruments.size(); ++i) {
      const int instrument = static_cast<int>(i);
      const StepBar* bar = timeline_->barOf(instrument, k);  // только бар текущего шага (INV-1)
      if (bar == nullptr) continue;
      strategy_.onBar(instrument, *bar);
      updateBarsInPosition(instrument);
      const std::vector<dsl::Intent> intents = strategy_.evaluate(
          instrument, *bar, spec_.instruments[i].lotSize, spec_.instruments[i].priceStep, state(instrument));
      for (const dsl::Intent& intent : intents) handle(k, intent);
    }
  }

  // Раз на каждый бар бумаги, до evaluate: баров с открытия позиции (0 - открыта на этом баре); позиции нет - сброс.
  void updateBarsInPosition(int instrument) {
    auto& bars = barsInPosition_[static_cast<std::size_t>(instrument)];
    if (engine_.portfolio().lots(instrument) > 0) {
      bars = bars ? *bars + 1 : 0;
    } else {
      bars.reset();
    }
  }

  // Чтение состояния на закрытии бара, без побочных эффектов.
  dsl::StateView state(int instrument) const {
    dsl::StateView s;
    const Portfolio& p = engine_.portfolio();
    s.positionLots = p.lots(instrument);
    s.cash = p.cash().toDouble();
    s.equity = engine_.equity().back().equity.toDouble();
    if (s.positionLots > 0) {
      s.entryPrice = p.avgPrice(instrument).toDouble();
      s.barsInPosition = barsInPosition_[static_cast<std::size_t>(instrument)];
    }
    return s;
  }

  void handle(std::int64_t k, const dsl::Intent& intent) {
    if (intent.cancelAll) {
      for (auto it = book_.begin(); it != book_.end();) {
        const Order* o = order(it->second);
        if (o == nullptr || !isActive(*o)) {
          it = book_.erase(it);
        } else if (o->spec.instrument == intent.instrument) {
          apply(k, CancelCmd{o->id, Reason::CancelledByUser}, intent.ruleName, intent.because);
          it = book_.erase(it);
        } else {
          ++it;
        }
      }
      return;
    }
    const auto key = std::make_pair(intent.ruleName, intent.instrument);
    const auto found = book_.find(key);
    if (found != book_.end()) {
      const Order* old = order(found->second);
      if (old != nullptr && isActive(*old)) {
        if (sameSpec(old->spec, intent.order)) return;  // та же заявка уже стоит
        // Замена атомарна (ReplaceCmd, причина RuleReplaced): отклонённая новая заявка не отнимает у правила прежнюю.
        const CommandResult replaced =
            apply(k, ReplaceCmd{old->id, PlaceCmd{intent.order, std::nullopt}}, intent.ruleName, intent.because);
        if (replaced.accepted) found->second = replaced.orderIds.front();
        return;
      }
      book_.erase(found);
    }
    const CommandResult result = apply(k, PlaceCmd{intent.order, std::nullopt}, intent.ruleName, intent.because);
    if (result.accepted) book_[key] = result.orderIds.front();
  }

  void collect(RunOutput& out, bool invalid) {
    out.orders = engine_.orders();
    out.fills = engine_.fills();
    out.equity = engine_.equity();
    out.roundTrips = engine_.portfolio().roundTrips();
    out.positionValuesByStep = engine_.positionValues();
    out.journal = journal_;
    out.finalEquity = out.equity.back().equity;
    out.lastStep = out.equity.back().step;
    out.status = invalid ? RunStatus::Invalid
                         : (engine_.status() == SessionStatus::Bankrupt ? RunStatus::BankruptStop
                                                                        : RunStatus::Completed);
    for (const Order& o : out.orders) {
      if (o.status == OrderStatus::Rejected) ++out.rejections;
      if (o.filledLots > 0 && o.filledLots < o.spec.lots) ++out.partialFills;
    }
  }

  const SessionSpec& spec_;
  std::shared_ptr<const Timeline> timeline_;
  SessionEngine engine_;
  dsl::StrategyInstance strategy_;
  const Progress& progress_;
  const detail::StepHook& hook_;
  std::int64_t lastStep_;
  std::map<std::pair<std::string, int>, std::int64_t> book_;  // (правило, бумага) -> id активной заявки
  std::vector<std::optional<std::int64_t>> barsInPosition_;
  std::vector<JournalEntry> journal_;
  std::int64_t seq_ = 0;
  std::int64_t reportedDone_ = -1;
};

}  // namespace

namespace detail {

RunOutput runBacktestWithHook(const SessionSpec& spec, std::shared_ptr<const Timeline> timeline,
                              const dsl::Program& program, const std::vector<dsl::ResolvedParam>& params,
                              const dsl::ParamValues& overrides, dsl::Level staticLevel, const Progress& progress,
                              const StepHook& hook) {
  if (!timeline) throw std::invalid_argument("runBacktest: нет шкалы времени");
  const std::int64_t lastStep = timeline->totalSteps() - 1;
  Runner runner(spec, std::move(timeline), program, params, overrides, progress, hook, lastStep);
  return runner.run(staticLevel);
}

RunOutput runBacktestToStep(const SessionSpec& spec, std::shared_ptr<const Timeline> timeline,
                            const dsl::Program& program, const std::vector<dsl::ResolvedParam>& params,
                            const dsl::ParamValues& overrides, dsl::Level staticLevel, std::int64_t lastStep) {
  if (!timeline) throw std::invalid_argument("runBacktestToStep: нет шкалы времени");
  if (lastStep < 0 || lastStep >= timeline->totalSteps()) {
    throw std::invalid_argument("runBacktestToStep: lastStep вне шкалы времени");
  }
  const Progress noProgress;
  const StepHook noHook;
  Runner runner(spec, std::move(timeline), program, params, overrides, noProgress, noHook, lastStep);
  return runner.run(staticLevel);
}

}  // namespace detail

RunOutput runBacktest(const SessionSpec& spec, std::shared_ptr<const Timeline> timeline, const dsl::Program& program,
                      const std::vector<dsl::ResolvedParam>& params, const dsl::ParamValues& overrides,
                      dsl::Level staticLevel, const Progress& progress) {
  return detail::runBacktestWithHook(spec, std::move(timeline), program, params, overrides, staticLevel, progress, {});
}

}  // namespace core
