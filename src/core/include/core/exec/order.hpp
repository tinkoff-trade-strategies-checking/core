#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp"  // Side

// Заявки и сделки модели исполнения (ФТ-7…ФТ-10, INV-2, INV-3). Деньги - Decimal (INV-6).

namespace core {

enum class OrderType { Market, Limit, Stop, StopLimit, TakeProfit, TrailingStop };
enum class TimeInForce { Day, Gtc };
enum class OrderStatus { Active, PartiallyFilled, Filled, Cancelled, Expired, Rejected };
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
// limitPrice - цена лимитной и стоп-лимитной заявок и цена тейк-профита; stopPrice - цена стопа и стоп-лимита.
struct OrderSpec {
  int instrument = 0;
  Side side = Side::Buy;
  OrderType type = OrderType::Market;
  std::int64_t lots = 0;
  std::optional<Decimal> limitPrice, stopPrice, trailingPercent;
  TimeInForce tif = TimeInForce::Gtc;
};

// Заявка в журнале сессии. -1 в ocoSiblingId и closedStep - «нет».
struct Order {
  std::int64_t id = 0;
  OrderSpec spec;
  std::int64_t placedStep = 0;  // шаг подачи; исполнение возможно не раньше шага placedStep + 1 (INV-2)
  OrderStatus status = OrderStatus::Active;
  Reason reason = Reason::None;
  std::string invalidField;  // поле, из-за которого InvalidParams
  std::int64_t filledLots = 0;
  std::int64_t ocoSiblingId = -1;
  std::string ruleName, because;  // правило стратегии и пояснение (для заявок алгоритма)
  std::optional<Decimal> trailingPeak, trailingStopLevel;
  bool triggered = false;        // стоп-лимит сработал
  std::int64_t closedStep = -1;  // шаг, на котором заявка стала неактивной
};

// Исполнение. price - цена за единицу бумаги, basePrice - цена до спреда и проскальзывания.
struct Fill {
  std::int64_t orderId = 0, step = 0;
  int instrument = 0;
  Side side = Side::Buy;
  std::int64_t lots = 0;
  Decimal price, basePrice, value, commission, fee, spreadCost, slippageCost;
  bool stopAndTakeSameBar = false;  // ФТ-9
};

}  // namespace core
