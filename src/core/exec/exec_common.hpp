#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "core/exec/costs.hpp"
#include "core/exec/execution.hpp"

// Общее для обработки рыночных и условных заявок (внутренний заголовок области
// exec).

namespace core {

struct Quote {
  Decimal Price;
  Amounts Amts;
};

// Количество, которое можно исполнить, и причина снятия остатка (None -
// исполнена вся заявка). Ограничения применяются по очереди: ликвидность,
// позиция (продажа), деньги (покупка); причина - последнее ограничение, которое
// уменьшило количество.
template <class QuoteFn>
inline std::pair<std::int64_t, Reason>
chooseQuantity(Side Side, std::int64_t Remaining,
               const std::optional<std::int64_t> &LiquidityCap,
               std::int64_t Position, const Decimal &Cash, QuoteFn &&QuoteOf) {
  std::int64_t Q = Remaining;
  Reason Reason = Reason::None;
  if (LiquidityCap && *LiquidityCap < Q) {
    Q = *LiquidityCap;
    Reason = Reason::LiquidityLimit;
  }
  if (Side == Side::Sell) {
    if (Position < Q) {
      Q = Position;
      Reason = Reason::InsufficientPosition;
    }
    return {Q, Reason};
  }
  // Покупка: наибольшее q, при котором хватает денег. Стоимость не убывает с
  // ростом q, поэтому поиск двоичный; цена пересчитывается для каждого q
  // (проскальзывание зависит от q).
  auto Fits = [&](std::int64_t N) {
    try {
      const Quote X = QuoteOf(N);
      return X.Amts.Value + X.Amts.Commission + X.Amts.Fee <= Cash;
    } catch (const DecimalOverflow &) {
      return false;
    }
  };
  if (Q > 0 && !Fits(Q)) {
    std::int64_t Lo = 0, Hi = Q - 1;
    while (Lo < Hi) {
      const std::int64_t Mid = Lo + (Hi - Lo + 1) / 2;
      if (Fits(Mid)) {
        Lo = Mid;
      } else {
        Hi = Mid - 1;
      }
    }
    Q = Lo;
    Reason = Reason::InsufficientCash;
  }
  return {Q, Reason};
}

// Проводит исполнение q лотов через портфель и дописывает сделку; возвращает её
// (ссылка действует до следующей записи).
inline Fill &recordFill(Portfolio &Portfolio, std::vector<Fill> &Fills,
                        const Order &O, std::int64_t Step, std::int64_t Q,
                        const Quote &X, const Decimal &Base,
                        const Instrument &Inst, const CostConfig &Costs) {
  const CostSplit Split = costSplit(Base, X.Price, Q, Inst.LotSize, Costs);
  Portfolio.applyFill(O.Spec.Instrument, O.Spec.OrderSide, Q, X.Price,
                      X.Amts.Value, X.Amts.Commission, X.Amts.Fee, Step, O.Id);
  Fill F;
  F.OrderId = O.Id;
  F.Step = Step;
  F.Instrument = O.Spec.Instrument;
  F.OrderSide = O.Spec.OrderSide;
  F.Lots = Q;
  F.Price = X.Price;
  F.BasePrice = Base;
  F.Value = X.Amts.Value;
  F.Commission = X.Amts.Commission;
  F.Fee = X.Amts.Fee;
  F.SpreadCost = Split.SpreadCost;
  F.SlippageCost = Split.SlippageCost;
  F.Seq = static_cast<std::int64_t>(Fills.size());
  Fills.push_back(F);
  return Fills.back();
}

} // namespace core
