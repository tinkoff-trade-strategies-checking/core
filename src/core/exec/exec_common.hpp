#pragma once

#include <algorithm>
#include <cstddef>
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

// Суммарная рыночная стоимость (со знаком) и валовая экспозиция (по модулю)
// всех позиций портфеля, кроме SkipInstr, по последним close <= Step.
inline std::pair<Decimal, Decimal>
otherExposure(const Portfolio &Folio,
              const std::vector<Instrument> &Instruments, const Timeline &Line,
              std::int64_t Step, int SkipInstr) {
  Decimal ValueSum;
  Decimal GrossSum;
  for (std::size_t I = 0; I < Instruments.size(); ++I) {
    if (static_cast<int>(I) == SkipInstr)
      continue;
    const std::int64_t LotsI = Folio.lots(static_cast<int>(I));
    if (LotsI == 0)
      continue;
    const StepBar *BarI = Line.lastBarOf(static_cast<int>(I), Step);
    if (BarI == nullptr)
      continue;
    const Decimal ValI =
        Portfolio::positionValue(LotsI, Instruments[I].LotSize, BarI->Close);
    ValueSum += ValI;
    GrossSum += ValI.abs();
  }
  return {ValueSum, GrossSum};
}

// Выбор исполняемого количества лотов в маржинальном / шортовом режиме
// (core#82, core#83): проверяет ликвидность, разрешение шорта и требование
// начальной маржи Dнач.
template <class QuoteFn>
inline std::pair<std::int64_t, Reason> chooseMarginQuantity(
    Side OrderSide, std::int64_t Remaining,
    const std::optional<std::int64_t> &LiquidityCap, std::int64_t Position,
    const Decimal &Cash, std::int64_t LotSize, const Decimal &OtherValue,
    const Decimal &OtherGross, const MarginConfig &Margin, QuoteFn &&QuoteOf) {
  std::int64_t Q = Remaining;
  Reason ReasonCode = Reason::None;
  if (LiquidityCap && *LiquidityCap < Q) {
    Q = *LiquidityCap;
    ReasonCode = Reason::LiquidityLimit;
  }
  if (OrderSide == Side::Sell && !Margin.AllowShort) {
    const std::int64_t MaxSell = Position > 0 ? Position : 0;
    if (MaxSell < Q) {
      Q = MaxSell;
      ReasonCode = Reason::InsufficientPosition;
    }
  }
  const Decimal DInit = Margin.effectiveInitialMarginRatio();
  auto Fits = [&](std::int64_t N) {
    const bool PurelyReducing =
        (OrderSide == Side::Buy && Position < 0 && N <= -Position) ||
        (OrderSide == Side::Sell && Position > 0 && N <= Position);
    if (PurelyReducing)
      return true;
    try {
      const Quote X = QuoteOf(N);
      const Decimal NewCash =
          (OrderSide == Side::Buy)
              ? (Cash - X.Amts.Value - X.Amts.Commission - X.Amts.Fee)
              : (Cash + X.Amts.Value - X.Amts.Commission - X.Amts.Fee);
      const std::int64_t NewLots =
          (OrderSide == Side::Buy) ? (Position + N) : (Position - N);
      const Decimal NewInstrVal =
          Portfolio::positionValue(NewLots, LotSize, X.Price);
      const Decimal NewEquity = NewCash + OtherValue + NewInstrVal;
      const Decimal NewGross = OtherGross + NewInstrVal.abs();
      const Decimal Req = NewGross.mul(DInit, 2, RoundingMode::CEILING);
      return NewEquity.sign() > 0 && NewEquity >= Req;
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
    ReasonCode = Reason::InsufficientMargin;
  }
  return {Q, ReasonCode};
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

// Добавляет id заявки в changed текущего шага без дублей.
inline void markChanged(std::vector<std::int64_t> &Changed,
                        std::size_t ChangedFrom, std::int64_t Id) {
  if (std::find(Changed.begin() + static_cast<std::ptrdiff_t>(ChangedFrom),
                Changed.end(), Id) == Changed.end()) {
    Changed.push_back(Id);
  }
}

// Когда позиция по бумаге обнуляется (position == 0), немедленно внутри шага
// снимает все оставшиеся активные заявки на продажу этой бумаги с причиной
// PositionClosed (INV-3, `decisions.md`, «Заявки», `dsl.md` п. 6).
inline void cancelSellOrdersOnPositionClosed(std::vector<Order> &Orders,
                                             int Instrument, std::int64_t Step,
                                             std::vector<std::int64_t> &Changed,
                                             std::size_t ChangedFrom) {
  for (Order &O : Orders) {
    if (O.Spec.Instrument != Instrument || O.Spec.OrderSide != Side::Sell)
      continue;
    if (O.PlacedStep >= Step)
      continue; // INV-2: заявка, поданная на этом шаге, ещё не действует
    if (O.Status != OrderStatus::Active &&
        O.Status != OrderStatus::PartiallyFilled)
      continue;
    O.Status = OrderStatus::Cancelled;
    O.ReasonCode = Reason::PositionClosed;
    O.ClosedStep = Step;
    markChanged(Changed, ChangedFrom, O.Id);
  }
}

} // namespace core
