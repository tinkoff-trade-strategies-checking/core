#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "core/exec/costs.hpp"
#include "core/exec/execution.hpp"

// Общее для обработки рыночных и условных заявок (внутренний заголовок области exec).

namespace core {

struct Quote {
  Decimal price;
  Amounts amounts;
};

// Количество, которое можно исполнить, и причина снятия остатка (None - исполнена вся заявка).
// Ограничения применяются по очереди: ликвидность, позиция (продажа), деньги (покупка); причина -
// последнее ограничение, которое уменьшило количество.
template <class QuoteFn>
inline std::pair<std::int64_t, Reason> chooseQuantity(Side side, std::int64_t remaining,
                                               const std::optional<std::int64_t>& liquidityCap, std::int64_t position,
                                               const Decimal& cash, QuoteFn&& quote) {
  std::int64_t q = remaining;
  Reason reason = Reason::None;
  if (liquidityCap && *liquidityCap < q) {
    q = *liquidityCap;
    reason = Reason::LiquidityLimit;
  }
  if (side == Side::Sell) {
    if (position < q) {
      q = position;
      reason = Reason::InsufficientPosition;
    }
    return {q, reason};
  }
  // Покупка: наибольшее q, при котором хватает денег. Стоимость не убывает с ростом q, поэтому поиск двоичный;
  // цена пересчитывается для каждого q (проскальзывание зависит от q).
  auto fits = [&](std::int64_t n) {
    try {
      const Quote x = quote(n);
      return x.amounts.value + x.amounts.commission + x.amounts.fee <= cash;
    } catch (const DecimalOverflow&) {
      return false;
    }
  };
  if (q > 0 && !fits(q)) {
    std::int64_t lo = 0, hi = q - 1;
    while (lo < hi) {
      const std::int64_t mid = lo + (hi - lo + 1) / 2;
      if (fits(mid)) {
        lo = mid;
      } else {
        hi = mid - 1;
      }
    }
    q = lo;
    reason = Reason::InsufficientCash;
  }
  return {q, reason};
}

// Проводит исполнение q лотов через портфель и дописывает сделку; возвращает её (ссылка действует до следующей записи).
inline Fill& recordFill(Portfolio& portfolio, std::vector<Fill>& fills, const Order& o, std::int64_t step,
                        std::int64_t q, const Quote& x, const Decimal& base, const Instrument& inst,
                        const CostConfig& costs) {
  const CostSplit split = costSplit(base, x.price, q, inst.lotSize, costs);
  portfolio.applyFill(o.spec.instrument, o.spec.side, q, x.price, x.amounts.value, x.amounts.commission,
                      x.amounts.fee, step, o.id);
  Fill f;
  f.orderId = o.id;
  f.step = step;
  f.instrument = o.spec.instrument;
  f.side = o.spec.side;
  f.lots = q;
  f.price = x.price;
  f.basePrice = base;
  f.value = x.amounts.value;
  f.commission = x.amounts.commission;
  f.fee = x.amounts.fee;
  f.spreadCost = split.spreadCost;
  f.slippageCost = split.slippageCost;
  fills.push_back(f);
  return fills.back();
}

}  // namespace core
