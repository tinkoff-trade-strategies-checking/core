#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp" // Side

// Заявки и сделки модели исполнения (ФТ-7…ФТ-10, INV-2, INV-3). Деньги -
// Decimal (INV-6).

namespace core {

enum class OrderType {
  Market,
  Limit,
  Stop,
  StopLimit,
  TakeProfit,
  TrailingStop
};
enum class TimeInForce { Day, Gtc };
enum class OrderStatus {
  Active,
  PartiallyFilled,
  Filled,
  Cancelled,
  Expired,
  Rejected
};
enum class Reason {
  None,
  InvalidParams,
  InsufficientCash,
  InsufficientPosition,
  SessionNotActive,
  OrderNotActive,
  LiquidityLimit,
  OcoSiblingFilled,
  PositionClosed,
  Replaced,
  SessionEnd,
  CancelledByUser,
  RuleReplaced,
  DayExpired
};

// Параметры заявки при подаче. instrument - индекс в списке бумаг сессии.
// limitPrice - цена лимитной и стоп-лимитной заявок и цена тейк-профита;
// stopPrice - цена стопа и стоп-лимита.
struct OrderSpec {
  int Instrument = 0;
  Side OrderSide = Side::Buy;
  OrderType Type = OrderType::Market;
  std::int64_t Lots = 0;
  std::optional<Decimal> LimitPrice, StopPrice, TrailingPercent;
  TimeInForce Tif = TimeInForce::Gtc;
};

// Заявка в журнале сессии. -1 в ocoSiblingId и closedStep - «нет».
struct Order {
  std::int64_t Id = 0;
  OrderSpec Spec;
  std::int64_t PlacedStep = 0; // шаг подачи; исполнение возможно не раньше шага
                               // placedStep + 1 (INV-2)
  OrderStatus Status = OrderStatus::Active;
  Reason ReasonCode = Reason::None;
  std::string InvalidField; // поле, из-за которого InvalidParams
  std::int64_t FilledLots = 0;
  std::int64_t OcoSiblingId = -1;
  std::string RuleName,
      Because; // правило стратегии и пояснение (для заявок алгоритма)
  std::optional<Decimal> TrailingPeak, TrailingStopLevel;
  bool Triggered = false; // стоп-лимит сработал
  std::int64_t ClosedStep = -1; // шаг, на котором заявка стала неактивной
};

// Исполнение. price - цена за единицу бумаги, basePrice - цена до спреда и
// проскальзывания.
struct Fill {
  std::int64_t OrderId = 0, Step = 0;
  int Instrument = 0;
  Side OrderSide = Side::Buy;
  std::int64_t Lots = 0;
  Decimal Price, BasePrice, Value, Commission, Fee, SpreadCost, SlippageCost;
  bool StopAndTakeSameBar = false; // ФТ-9
};

} // namespace core
