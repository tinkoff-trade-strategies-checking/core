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

bool live(const Order &O) {
  return O.Status == OrderStatus::Active ||
         O.Status == OrderStatus::PartiallyFilled;
}

bool isStopType(const Order &O) {
  return O.Spec.Type == OrderType::Stop ||
         O.Spec.Type == OrderType::TrailingStop ||
         (O.Spec.Type == OrderType::StopLimit && !O.Triggered);
}

// Лимитная группа: лимит, тейк-профит и стоп-лимит, сработавший раньше.
bool isLimitType(const Order &O) {
  return O.Spec.Type == OrderType::Limit ||
         O.Spec.Type == OrderType::TakeProfit ||
         (O.Spec.Type == OrderType::StopLimit && O.Triggered);
}

Order *findOrder(std::vector<Order> &Orders, std::int64_t Id) {
  for (Order &O : Orders) {
    if (O.Id == Id)
      return &O;
  }
  return nullptr;
}

struct Context {
  std::int64_t Step;
  const std::vector<Instrument> &Instruments;
  const CostConfig &Costs;
  const Timeline &Line;
  LiquidityBudget &Budget;
  std::vector<Order> &Orders;
  Portfolio &Folio;
  std::vector<Fill> &Fills;
  std::vector<std::int64_t> &Changed;
  std::size_t ChangedFrom; // с этой позиции changed - записи текущего шага: id
                           // в них не дублируются
  std::vector<std::size_t> ById; // индексы orders по возрастанию id
  std::vector<char>
      TriggeredNow; // стоп-лимит сработал на этом шаге (индекс как в orders)

  void markChanged(std::int64_t Id) {
    if (std::find(Changed.begin() + static_cast<std::ptrdiff_t>(ChangedFrom),
                  Changed.end(), Id) == Changed.end()) {
      Changed.push_back(Id);
    }
  }

  const Instrument &instrumentOf(const Order &O) const {
    return Instruments[static_cast<std::size_t>(O.Spec.Instrument)];
  }
};

void closeOrder(Context &C, Order &O, OrderStatus Status, Reason Reason) {
  O.Status = Status;
  O.ReasonCode = Reason;
  O.ClosedStep = C.Step;
  C.markChanged(O.Id);
}

// Исполнение любого объёма одной ноги OCO снимает вторую.
void cancelSibling(Context &C, const Order &O) {
  if (O.OcoSiblingId < 0)
    return;
  Order *Sibling = findOrder(C.Orders, O.OcoSiblingId);
  if (Sibling != nullptr && live(*Sibling))
    closeOrder(C, *Sibling, OrderStatus::Cancelled, Reason::OcoSiblingFilled);
}

// Тейк-профит (или вторая нога OCO-пары) на продажу той же бумаги, который этот
// бар тоже задел бы (для пометки ФТ-9).
bool takeWouldFill(const Context &C, const Order &Stop, const StepBar &Bar) {
  for (std::size_t I = 0; I < C.Orders.size(); ++I) {
    const Order &O = C.Orders[I];
    if (&O == &Stop || !live(O) || O.PlacedStep >= C.Step ||
        O.Spec.Instrument != Stop.Spec.Instrument)
      continue;
    const bool IsTakeOrOcoSibling =
        O.Spec.Type == OrderType::TakeProfit ||
        (Stop.OcoSiblingId >= 0 && O.Id == Stop.OcoSiblingId);
    if (!IsTakeOrOcoSibling || O.Spec.OrderSide != Side::Sell ||
        !isLimitType(O) || C.TriggeredNow[I] || !O.Spec.LimitPrice)
      continue;
    const Decimal &Level = *O.Spec.LimitPrice;
    if (Bar.Open >= Level || Bar.High > Level)
      return true;
  }
  return false;
}

struct Executed {
  std::int64_t Lots;
  Reason ReasonCode; // None - вся заявка исполнена; иначе ограничение,
                     // уменьшившее количество
};

// Исполняет остаток заявки по цене priceOf(q): лоты ограничены бюджетом
// ликвидности, позицией и деньгами. Дописывает сделку, списывает бюджет и
// снимает вторую ногу OCO; статус заявки не меняет.
template <class PriceFn>
Executed execute(Context &C, Order &O, const StepBar &Bar, const Decimal &Base,
                 PriceFn &&PriceOf, Fill **FillOut) {
  const Instrument &Inst = C.instrumentOf(O);
  const int Instrument = O.Spec.Instrument;
  auto QuoteAt = [&](std::int64_t Q) {
    const Decimal Price = PriceOf(Q);
    return Quote{Price, amounts(Price, Q, Inst.LotSize, C.Costs)};
  };
  const std::optional<std::int64_t> Cap =
      C.Budget.remaining(Instrument, Bar, C.Costs);
  const std::pair<std::int64_t, Reason> Chosen =
      chooseQuantity(O.Spec.OrderSide, O.Spec.Lots - O.FilledLots, Cap,
                     C.Folio.lots(Instrument), C.Folio.cash(), QuoteAt);
  const std::int64_t Q = Chosen.first;
  if (Q > 0) {
    const Quote X = QuoteAt(Q);
    Fill &F =
        recordFill(C.Folio, C.Fills, O, C.Step, Q, X, Base, Inst, C.Costs);
    if (FillOut != nullptr)
      *FillOut = &F;
    O.FilledLots += Q;
    C.Budget.consume(Instrument, Q);
    C.markChanged(O.Id);
    cancelSibling(C, O);
  }
  return Executed{Q, Chosen.second};
}

// Уровень трейлинга для пика: peak·(1 − d/100), вниз до шага цены (один
// направленный режим).
Decimal trailingLevel(const Decimal &Peak, const Decimal &Percent,
                      const Decimal &PriceStep) {
  const Decimal Hundred = Decimal::fromUnits(100);
  return Peak
      .mulDiv(Hundred - Percent, Hundred, Decimal::kScale, RoundingMode::FLOOR)
      .roundToStep(PriceStep, RoundingMode::FLOOR);
}

// Пик при подаче - close бара подачи (последнего бара бумаги не позже шага
// подачи). false - бара нет.
bool ensureTrailing(const Context &C, Order &O) {
  if (O.TrailingPeak && O.TrailingStopLevel)
    return true;
  const StepBar *Placed = C.Line.lastBarOf(O.Spec.Instrument, O.PlacedStep);
  if (Placed == nullptr || !O.Spec.TrailingPercent)
    return false;
  O.TrailingPeak = Placed->Close;
  O.TrailingStopLevel = trailingLevel(*O.TrailingPeak, *O.Spec.TrailingPercent,
                                      C.instrumentOf(O).PriceStep);
  return true;
}

void runStops(Context &C) {
  for (const std::size_t Idx : C.ById) {
    Order &O = C.Orders[Idx];
    if (!live(O) || O.PlacedStep >= C.Step || !isStopType(O))
      continue;
    const StepBar *Bar = C.Line.barOf(O.Spec.Instrument, C.Step);
    if (Bar == nullptr)
      continue; // ждёт собственного бара бумаги

    const bool Buy = O.Spec.OrderSide == Side::Buy;
    const bool Trailing = O.Spec.Type == OrderType::TrailingStop;
    if (Trailing) {
      if (!ensureTrailing(C, O))
        continue;
    } else if (!O.Spec.StopPrice) {
      continue;
    }
    const Decimal Level = Trailing ? *O.TrailingStopLevel : *O.Spec.StopPrice;

    const bool Triggered = Buy ? (Bar->Open >= Level || Bar->High >= Level)
                               : (Bar->Open <= Level || Bar->Low <= Level);
    if (!Triggered) {
      if (Trailing) {
        // Уровень для следующего бара считается по пику с учётом этого бара.
        if (Bar->High > *O.TrailingPeak) {
          O.TrailingPeak = Bar->High;
          C.markChanged(O.Id);
        }
        O.TrailingStopLevel =
            trailingLevel(*O.TrailingPeak, *O.Spec.TrailingPercent,
                          C.instrumentOf(O).PriceStep);
      }
      continue;
    }
    if (O.Spec.Type == OrderType::StopLimit) {
      // Не исполняется на баре срабатывания: с шага b + 1 действует как
      // лимитная.
      O.Triggered = true;
      C.TriggeredNow[Idx] = 1;
      C.markChanged(O.Id);
      continue;
    }

    // Стоп исполняется на этом же баре как рыночная заявка от base = max(O, S)
    // (покупка) или min(O, S) (продажа).
    const Decimal Base =
        Buy ? std::max(Bar->Open, Level) : std::min(Bar->Open, Level);
    const bool Flag =
        !Buy && takeWouldFill(C, O, *Bar); // до снятия второй ноги OCO
    const Instrument &Inst = C.instrumentOf(O);
    Fill *Fill = nullptr;
    const Executed E = execute(
        C, O, *Bar, Base,
        [&](std::int64_t Q) {
          return fillPrice(O.Spec.OrderSide, Base, Q, Bar->VolumeLots, C.Costs,
                           Inst.PriceStep);
        },
        &Fill);
    if (Fill != nullptr && Flag)
      Fill->StopAndTakeSameBar = true;
    // Остаток стопа не переносится: исполнен целиком или снят с причиной
    // (ликвидность, позиция, деньги).
    if (E.ReasonCode == Reason::None) {
      closeOrder(C, O, OrderStatus::Filled, Reason::None);
    } else {
      closeOrder(C, O, OrderStatus::Cancelled, E.ReasonCode);
    }
  }
}

void runLimits(Context &C) {
  for (const std::size_t Idx : C.ById) {
    Order &O = C.Orders[Idx];
    if (!live(O) || O.PlacedStep >= C.Step || !isLimitType(O) ||
        C.TriggeredNow[Idx] || !O.Spec.LimitPrice)
      continue;
    const StepBar *Bar = C.Line.barOf(O.Spec.Instrument, C.Step);
    if (Bar == nullptr)
      continue;

    const Decimal &Limit = *O.Spec.LimitPrice;
    const bool Buy = O.Spec.OrderSide == Side::Buy;
    // Открытие лучше или равно лимиту - исполнение от open; иначе бар должен
    // пройти уровень (касание не исполняет).
    const bool AtOpen = Buy ? Bar->Open <= Limit : Bar->Open >= Limit;
    const bool Crossed = AtOpen || (Buy ? Bar->Low < Limit : Bar->High > Limit);
    if (!Crossed)
      continue;

    const Instrument &Inst = C.instrumentOf(O);
    const Decimal Base = AtOpen ? Bar->Open : Limit;
    Fill *Fill = nullptr;
    const Executed E = execute(
        C, O, *Bar, Base,
        [&](std::int64_t Q) {
          if (!AtOpen)
            return Limit;
          const Decimal P = fillPrice(O.Spec.OrderSide, Bar->Open, Q,
                                      Bar->VolumeLots, C.Costs, Inst.PriceStep);
          return Buy ? std::min(Limit, P) : std::max(Limit, P);
        },
        &Fill);
    // Исполнение по уровню лимита идёт без спреда и проскальзывания: в отчёте о
    // сделке их стоимость нулевая.
    if (Fill != nullptr && !AtOpen) {
      Fill->SpreadCost = Fill->SlippageCost = Decimal::zero();
    } else if (Fill != nullptr && Fill->SlippageCost.sign() < 0) {
      // Цена от open упёрлась в лимит: вся разница с базовой ценой - спред,
      // отрицательного проскальзывания нет.
      Fill->SpreadCost += Fill->SlippageCost;
      Fill->SlippageCost = Decimal::zero();
    }

    if (O.FilledLots >= O.Spec.Lots) {
      closeOrder(C, O, OrderStatus::Filled, Reason::None);
    } else if (E.ReasonCode == Reason::LiquidityLimit) {
      // Остаток остаётся активным до конца срока (`decisions.md`, «Заявки»).
      if (E.Lots > 0)
        O.Status = OrderStatus::PartiallyFilled;
    } else {
      closeOrder(C, O, OrderStatus::Cancelled,
                 E.ReasonCode); // не хватило денег или позиции
    }
  }
}

// Срок «день»: заявка действует весь торговый день шага s + 1 и снимается после
// его последнего шага.
void expireDayOrders(Context &C) {
  if (!C.Line.isLastStepOfDay(C.Step))
    return;
  const std::int32_t Today = C.Line.tradingDayOf(C.Step);
  for (const std::size_t Idx : C.ById) {
    Order &O = C.Orders[Idx];
    if (!live(O) || O.Spec.Tif != TimeInForce::Day ||
        O.PlacedStep + 1 >= C.Line.totalSteps())
      continue;
    if (C.Line.tradingDayOf(O.PlacedStep + 1) <= Today)
      closeOrder(C, O, OrderStatus::Expired, Reason::DayExpired);
  }
}

} // namespace

void ExecutionEngine::processConditional(
    std::int64_t Step, const Timeline &Timeline, LiquidityBudget &Budget,
    std::vector<Order> &Orders, Portfolio &Portfolio, std::vector<Fill> &Fills,
    std::vector<std::int64_t> &Changed) const {
  Context C{Step,      Instruments, Costs,   Timeline,       Budget, Orders,
            Portfolio, Fills,       Changed, Changed.size(), {},     {}};
  C.ById.resize(Orders.size());
  std::iota(C.ById.begin(), C.ById.end(), std::size_t{0});
  std::stable_sort(C.ById.begin(), C.ById.end(),
                   [&](std::size_t A, std::size_t B) {
                     return Orders[A].Id < Orders[B].Id;
                   });
  C.TriggeredNow.assign(Orders.size(), 0);

  runStops(C);
  runLimits(C);
  expireDayOrders(C);
}

bool cancelOrder(std::vector<Order> &Orders, std::int64_t OrderId,
                 Reason Reason, std::int64_t Step,
                 std::vector<std::int64_t> &Changed) {
  Order *O = findOrder(Orders, OrderId);
  if (O == nullptr || !live(*O))
    return false;
  auto Close = [&](Order &X) {
    X.Status = OrderStatus::Cancelled;
    X.ReasonCode = Reason;
    X.ClosedStep = Step;
    Changed.push_back(X.Id);
  };
  Close(*O);
  if (O->OcoSiblingId >= 0) {
    Order *Sibling = findOrder(Orders, O->OcoSiblingId);
    if (Sibling != nullptr && live(*Sibling))
      Close(*Sibling);
  }
  return true;
}

} // namespace core
