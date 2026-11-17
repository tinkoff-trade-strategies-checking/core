#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <utility>

#include "core/exec/execution.hpp"
#include "exec_common.hpp"

// Условные заявки (ФТ-7, ФТ-9): лимит, стоп, стоп-лимит, тейк-профит, трейлинг,
// OCO, срок «день». Заявка, поданная на шаге s, действует с шага s + 1 и ждёт
// бара своей бумаги (INV-2).

namespace core {

namespace {

bool live(const Order &o) {
  return o.status == OrderStatus::Active ||
         o.status == OrderStatus::PartiallyFilled;
}

bool isStopType(const Order &o) {
  return o.spec.type == OrderType::Stop ||
         o.spec.type == OrderType::TrailingStop ||
         (o.spec.type == OrderType::StopLimit && !o.triggered);
}

// Лимитная группа: лимит, тейк-профит и стоп-лимит, сработавший раньше.
bool isLimitType(const Order &o) {
  return o.spec.type == OrderType::Limit ||
         o.spec.type == OrderType::TakeProfit ||
         (o.spec.type == OrderType::StopLimit && o.triggered);
}

Order *findOrder(std::vector<Order> &orders, std::int64_t id) {
  for (Order &o : orders) {
    if (o.id == id)
      return &o;
  }
  return nullptr;
}

struct Context {
  std::int64_t step;
  const std::vector<Instrument> &instruments;
  const CostConfig &costs;
  const Timeline &timeline;
  LiquidityBudget &budget;
  std::vector<Order> &orders;
  Portfolio &portfolio;
  std::vector<Fill> &fills;
  std::vector<std::int64_t> &changed;
  std::size_t changedFrom; // с этой позиции changed - записи текущего шага: id
                           // в них не дублируются
  std::vector<std::size_t> byId; // индексы orders по возрастанию id
  std::vector<char>
      triggeredNow; // стоп-лимит сработал на этом шаге (индекс как в orders)

  void markChanged(std::int64_t id) {
    if (std::find(changed.begin() + static_cast<std::ptrdiff_t>(changedFrom),
                  changed.end(), id) == changed.end()) {
      changed.push_back(id);
    }
  }

  const Instrument &instrumentOf(const Order &o) const {
    return instruments[static_cast<std::size_t>(o.spec.instrument)];
  }
};

void closeOrder(Context &c, Order &o, OrderStatus status, Reason reason) {
  o.status = status;
  o.reason = reason;
  o.closedStep = c.step;
  c.markChanged(o.id);
}

// Исполнение любого объёма одной ноги OCO снимает вторую.
void cancelSibling(Context &c, const Order &o) {
  if (o.ocoSiblingId < 0)
    return;
  Order *sibling = findOrder(c.orders, o.ocoSiblingId);
  if (sibling != nullptr && live(*sibling))
    closeOrder(c, *sibling, OrderStatus::Cancelled, Reason::OcoSiblingFilled);
}

// Лимитная заявка на продажу той же бумаги, которую этот бар тоже задел бы (для
// пометки ФТ-9).
bool sellLimitWouldFill(const Context &c, const Order &stop,
                        const StepBar &bar) {
  for (std::size_t i = 0; i < c.orders.size(); ++i) {
    const Order &o = c.orders[i];
    if (&o == &stop || !live(o) || o.placedStep >= c.step ||
        o.spec.instrument != stop.spec.instrument)
      continue;
    if (o.spec.side != Side::Sell || !isLimitType(o) || c.triggeredNow[i] ||
        !o.spec.limitPrice)
      continue;
    const Decimal &level = *o.spec.limitPrice;
    if (bar.open >= level || bar.high > level)
      return true;
  }
  return false;
}

struct Executed {
  std::int64_t lots;
  Reason reason; // None - вся заявка исполнена; иначе ограничение, уменьшившее
                 // количество
};

// Исполняет остаток заявки по цене priceOf(q): лоты ограничены бюджетом
// ликвидности, позицией и деньгами. Дописывает сделку, списывает бюджет и
// снимает вторую ногу OCO; статус заявки не меняет.
template <class PriceFn>
Executed execute(Context &c, Order &o, const StepBar &bar, const Decimal &base,
                 PriceFn &&priceOf, Fill **fillOut) {
  const Instrument &inst = c.instrumentOf(o);
  const int instrument = o.spec.instrument;
  auto quote = [&](std::int64_t q) {
    const Decimal price = priceOf(q);
    return Quote{price, amounts(price, q, inst.lotSize, c.costs)};
  };
  const std::optional<std::int64_t> cap =
      c.budget.remaining(instrument, bar, c.costs);
  const std::pair<std::int64_t, Reason> chosen =
      chooseQuantity(o.spec.side, o.spec.lots - o.filledLots, cap,
                     c.portfolio.lots(instrument), c.portfolio.cash(), quote);
  const std::int64_t q = chosen.first;
  if (q > 0) {
    const Quote x = quote(q);
    Fill &f =
        recordFill(c.portfolio, c.fills, o, c.step, q, x, base, inst, c.costs);
    if (fillOut != nullptr)
      *fillOut = &f;
    o.filledLots += q;
    c.budget.consume(instrument, q);
    c.markChanged(o.id);
    cancelSibling(c, o);
  }
  return Executed{q, chosen.second};
}

// Уровень трейлинга для пика: peak·(1 − d/100), вниз до шага цены (один
// направленный режим).
Decimal trailingLevel(const Decimal &peak, const Decimal &percent,
                      const Decimal &priceStep) {
  const Decimal hundred = Decimal::fromUnits(100);
  return peak
      .mulDiv(hundred - percent, hundred, Decimal::kScale, RoundingMode::FLOOR)
      .roundToStep(priceStep, RoundingMode::FLOOR);
}

// Пик при подаче - close бара подачи (последнего бара бумаги не позже шага
// подачи). false - бара нет.
bool ensureTrailing(const Context &c, Order &o) {
  if (o.trailingPeak && o.trailingStopLevel)
    return true;
  const StepBar *placed = c.timeline.lastBarOf(o.spec.instrument, o.placedStep);
  if (placed == nullptr || !o.spec.trailingPercent)
    return false;
  o.trailingPeak = placed->close;
  o.trailingStopLevel = trailingLevel(*o.trailingPeak, *o.spec.trailingPercent,
                                      c.instrumentOf(o).priceStep);
  return true;
}

void runStops(Context &c) {
  for (const std::size_t idx : c.byId) {
    Order &o = c.orders[idx];
    if (!live(o) || o.placedStep >= c.step || !isStopType(o))
      continue;
    const StepBar *bar = c.timeline.barOf(o.spec.instrument, c.step);
    if (bar == nullptr)
      continue; // ждёт собственного бара бумаги

    const bool buy = o.spec.side == Side::Buy;
    const bool trailing = o.spec.type == OrderType::TrailingStop;
    if (trailing) {
      if (!ensureTrailing(c, o))
        continue;
    } else if (!o.spec.stopPrice) {
      continue;
    }
    const Decimal level = trailing ? *o.trailingStopLevel : *o.spec.stopPrice;

    const bool triggered = buy ? (bar->open >= level || bar->high >= level)
                               : (bar->open <= level || bar->low <= level);
    if (!triggered) {
      if (trailing) {
        // Уровень для следующего бара считается по пику с учётом этого бара.
        if (bar->high > *o.trailingPeak) {
          o.trailingPeak = bar->high;
          c.markChanged(o.id);
        }
        o.trailingStopLevel =
            trailingLevel(*o.trailingPeak, *o.spec.trailingPercent,
                          c.instrumentOf(o).priceStep);
      }
      continue;
    }
    if (o.spec.type == OrderType::StopLimit) {
      // Не исполняется на баре срабатывания: с шага b + 1 действует как
      // лимитная.
      o.triggered = true;
      c.triggeredNow[idx] = 1;
      c.markChanged(o.id);
      continue;
    }

    // Стоп исполняется на этом же баре как рыночная заявка от base = max(O, S)
    // (покупка) или min(O, S) (продажа).
    const Decimal base =
        buy ? std::max(bar->open, level) : std::min(bar->open, level);
    const bool flag =
        !buy && sellLimitWouldFill(c, o, *bar); // до снятия второй ноги OCO
    const Instrument &inst = c.instrumentOf(o);
    Fill *fill = nullptr;
    const Executed e = execute(
        c, o, *bar, base,
        [&](std::int64_t q) {
          return fillPrice(o.spec.side, base, q, bar->volumeLots, c.costs,
                           inst.priceStep);
        },
        &fill);
    if (fill != nullptr && flag)
      fill->stopAndTakeSameBar = true;
    // Остаток стопа не переносится: исполнен целиком или снят с причиной
    // (ликвидность, позиция, деньги).
    if (e.reason == Reason::None) {
      closeOrder(c, o, OrderStatus::Filled, Reason::None);
    } else {
      closeOrder(c, o, OrderStatus::Cancelled, e.reason);
    }
  }
}

void runLimits(Context &c) {
  for (const std::size_t idx : c.byId) {
    Order &o = c.orders[idx];
    if (!live(o) || o.placedStep >= c.step || !isLimitType(o) ||
        c.triggeredNow[idx] || !o.spec.limitPrice)
      continue;
    const StepBar *bar = c.timeline.barOf(o.spec.instrument, c.step);
    if (bar == nullptr)
      continue;

    const Decimal &limit = *o.spec.limitPrice;
    const bool buy = o.spec.side == Side::Buy;
    // Открытие лучше или равно лимиту - исполнение от open; иначе бар должен
    // пройти уровень (касание не исполняет).
    const bool atOpen = buy ? bar->open <= limit : bar->open >= limit;
    const bool crossed = atOpen || (buy ? bar->low < limit : bar->high > limit);
    if (!crossed)
      continue;

    const Instrument &inst = c.instrumentOf(o);
    const Decimal base = atOpen ? bar->open : limit;
    Fill *fill = nullptr;
    const Executed e = execute(
        c, o, *bar, base,
        [&](std::int64_t q) {
          if (!atOpen)
            return limit;
          const Decimal p = fillPrice(o.spec.side, bar->open, q,
                                      bar->volumeLots, c.costs, inst.priceStep);
          return buy ? std::min(limit, p) : std::max(limit, p);
        },
        &fill);
    // Исполнение по уровню лимита идёт без спреда и проскальзывания: в отчёте о
    // сделке их стоимость нулевая.
    if (fill != nullptr && !atOpen) {
      fill->spreadCost = fill->slippageCost = Decimal::zero();
    } else if (fill != nullptr && fill->slippageCost.sign() < 0) {
      // Цена от open упёрлась в лимит: вся разница с базовой ценой - спред,
      // отрицательного проскальзывания нет.
      fill->spreadCost += fill->slippageCost;
      fill->slippageCost = Decimal::zero();
    }

    if (o.filledLots >= o.spec.lots) {
      closeOrder(c, o, OrderStatus::Filled, Reason::None);
    } else if (e.reason == Reason::LiquidityLimit) {
      // Остаток остаётся активным до конца срока (`decisions.md`, «Заявки»).
      if (e.lots > 0)
        o.status = OrderStatus::PartiallyFilled;
    } else {
      closeOrder(c, o, OrderStatus::Cancelled,
                 e.reason); // не хватило денег или позиции
    }
  }
}

// Срок «день»: заявка действует весь торговый день шага s + 1 и снимается после
// его последнего шага.
void expireDayOrders(Context &c) {
  if (!c.timeline.isLastStepOfDay(c.step))
    return;
  const std::int32_t today = c.timeline.tradingDayOf(c.step);
  for (const std::size_t idx : c.byId) {
    Order &o = c.orders[idx];
    if (!live(o) || o.spec.tif != TimeInForce::Day ||
        o.placedStep + 1 >= c.timeline.totalSteps())
      continue;
    if (c.timeline.tradingDayOf(o.placedStep + 1) <= today)
      closeOrder(c, o, OrderStatus::Expired, Reason::DayExpired);
  }
}

} // namespace

void ExecutionEngine::processConditional(
    std::int64_t step, const Timeline &timeline, LiquidityBudget &budget,
    std::vector<Order> &orders, Portfolio &portfolio, std::vector<Fill> &fills,
    std::vector<std::int64_t> &changed) const {
  Context c{step,      instruments_, costs_,  timeline,       budget, orders,
            portfolio, fills,        changed, changed.size(), {},     {}};
  c.byId.resize(orders.size());
  std::iota(c.byId.begin(), c.byId.end(), std::size_t{0});
  std::stable_sort(c.byId.begin(), c.byId.end(),
                   [&](std::size_t a, std::size_t b) {
                     return orders[a].id < orders[b].id;
                   });
  c.triggeredNow.assign(orders.size(), 0);

  runStops(c);
  runLimits(c);
  expireDayOrders(c);
}

bool cancelOrder(std::vector<Order> &orders, std::int64_t orderId,
                 Reason reason, std::int64_t step,
                 std::vector<std::int64_t> &changed) {
  Order *o = findOrder(orders, orderId);
  if (o == nullptr || !live(*o))
    return false;
  auto close = [&](Order &x) {
    x.status = OrderStatus::Cancelled;
    x.reason = reason;
    x.closedStep = step;
    changed.push_back(x.id);
  };
  close(*o);
  if (o->ocoSiblingId >= 0) {
    Order *sibling = findOrder(orders, o->ocoSiblingId);
    if (sibling != nullptr && live(*sibling))
      close(*sibling);
  }
  return true;
}

} // namespace core
