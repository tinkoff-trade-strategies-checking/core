#pragma once

#include <cstdint>
#include <vector>

#include "core/market/bar.hpp"
#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp"

// Симуляция биржевого стакана (Level 2) и ленты сделок (Time & Sales)
// (core#85). Детерминированная генерация без плавающей точки (INV-4, INV-6).

namespace core {

// Один ценовой уровень стакана (цена за единицу бумаги и объём в лотах).
struct OrderBookLevel {
  Decimal Price;
  std::int64_t Lots = 0;

  friend bool operator==(const OrderBookLevel &,
                         const OrderBookLevel &) = default;
};

// Снапшот стакана Level 2: Bids строго по убыванию, Asks строго по
// возрастанию.
struct OrderBookSnapshot {
  std::int64_t TimeUnixMs = 0;
  Decimal MidPrice;
  Decimal SpreadBps;
  std::vector<OrderBookLevel> Bids;
  std::vector<OrderBookLevel> Asks;

  friend bool operator==(const OrderBookSnapshot &,
                         const OrderBookSnapshot &) = default;
};

// Сделка из ленты обезличенных сделок (Time & Sales).
struct TapePrint {
  std::int64_t TimeUnixMs = 0;
  Decimal Price;
  std::int64_t Lots = 0;
  Side AggressorSide = Side::Buy;

  friend bool operator==(const TapePrint &, const TapePrint &) = default;
};

// Настройки синтетического стакана и ленты сделок.
// Нулевые SpreadPct и PriceStep заменяются на значения по умолчанию.
struct OrderBookConfig {
  int Depth = 10;
  Decimal SpreadPct;
  Decimal PriceStep;
  int TapePrintsPerBar = 6;

  friend bool operator==(const OrderBookConfig &,
                         const OrderBookConfig &) = default;
};

// Результат сведения рыночной заявки «проеданием» уровней стакана.
struct BookFillResult {
  Decimal AvgPrice;
  std::int64_t FilledLots = 0;
  std::int64_t RemainingLots = 0;
  Decimal PriceImpactBps;
  Decimal TotalValue;

  friend bool operator==(const BookFillResult &,
                         const BookFillResult &) = default;
};

// Детерминированно строит синтетический стакан Level 2 по бару OHLCV (INV-4,
// INV-6). Bids строго убывают, Asks строго возрастают, BestBid < BestAsk, цены
// кратны PriceStep, объёмы пропорциональны SourceBar.Volume.
OrderBookSnapshot buildOrderBook(const Bar &SourceBar,
                                 const OrderBookConfig &Cfg);

// Детерминированно синтезирует ленту сделок (Time & Sales) по бару OHLCV:
// траектория Open -> Low/High -> Close, цены в [Low, High], сумма лотов равна
// SourceBar.Volume, метки времени не убывают.
std::vector<TapePrint> synthesizeTape(const Bar &SourceBar,
                                      const OrderBookConfig &Cfg);

// Сводит заявку объёмом RequestedLots по уровням стакана (Buy по Asks, Sell по
// Bids). Возвращает точную VWAP цену, исполненные и неисполненные лоты и
// рыночное воздействие в базисных пунктах относительно лучшей цены стороны.
BookFillResult walkBookFill(const OrderBookSnapshot &Book, Side OrderSide,
                            std::int64_t RequestedLots);

} // namespace core
