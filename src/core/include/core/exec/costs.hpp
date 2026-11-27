#pragma once

#include <cstdint>
#include <optional>

#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp" // Side

// Издержки исполнения (INV-3): комиссия, биржевой сбор, спред, проскальзывание,
// лимит ликвидности. Округления - `decisions.md`, «Деньги и округление», п. 1,
// 3, 4.

namespace core {

struct CostConfig {
  Decimal CommissionRate, ExchangeFeeRate, Spread, SlippageKappa;
  // Доля объёма бара, доступная заявкам шага; nullopt - без лимита.
  std::optional<Decimal> LiquidityShare;

  // Значения по умолчанию: комиссия 0,003; сбор 0,0001; спред 0,001; κ = 0,1;
  // лимит ликвидности 0,10.
  static CostConfig defaults();
  // Все издержки нулевые, без лимита ликвидности.
  static CostConfig zero();
};

// Проверяет инварианты параметров издержек: ставки, спред и κ >= 0,
// LiquidityShare в (0; 1] (std::invalid_argument при нарушении).
void validateCostConfig(const CostConfig &Costs);

// Настройки маржинальной торговли и коротких позиций (core#82, core#83).
// По умолчанию полностью отключены: AllowShort = false, MaxLeverage = 1,
// все ставки и пороги = 0 (полная обратная совместимость).
struct MarginConfig {
  bool AllowShort = false;
  Decimal MaxLeverage = Decimal::one();
  // Годовая ставка переноса короткой позиции овернайт, % годовых (Actual/365).
  Decimal ShortBorrowAnnualPct;
  // Годовая ставка маржинального займа по отрицательным деньгам, % годовых.
  Decimal MarginLoanAnnualPct;
  // Начальная маржа Dнач в (0; 1]; 0 означает 1 / MaxLeverage.
  Decimal InitialMarginRatio;
  // Минимальная маржа Dmin в [0; Dнач]; 0 отключает принудительное закрытие.
  Decimal MaintenanceMarginRatio;

  static MarginConfig disabled() { return MarginConfig{}; }

  // Эффективная ставка Dнач: явная InitialMarginRatio, иначе 1 / MaxLeverage.
  Decimal effectiveInitialMarginRatio() const;

  // Включён ли маржинальный/шортовый контур.
  bool isMarginEnabled() const;
};

// Проверяет инварианты параметров маржинальной торговли
// (std::invalid_argument при нарушении).
void validateMarginConfig(const MarginConfig &Margin);

// Настройки аукционов открытия и закрытия MOEX (core#84). По умолчанию
// отключены.
struct AuctionConfig {
  bool EnableOpeningAuction = false;
  bool EnableClosingAuction = false;
};

// Расширенные правила исполнения заявок (core#84): ценовые планки и аукционы.
// По умолчанию полностью отключены (обратная совместимость).
struct ExecutionRules {
  // Ценовой коридор ±PriceBandPct% от close предыдущего торгового дня
  // (например, 10 или 40). 0 = отключено.
  Decimal PriceBandPct;
  AuctionConfig Auction{};

  static ExecutionRules defaults() { return ExecutionRules{}; }
  bool hasPriceBand() const { return PriceBandPct.sign() > 0; }
};

// Проверяет инварианты расширенных правил исполнения (std::invalid_argument).
void validateExecutionRules(const ExecutionRules &Rules);

// Цена исполнения рыночной заявки: frac = κ·lots/V (9 знаков HALF_UP); покупка
// m = 1 + s/2 + frac, продажа m = 1 − s/2 − frac; base·m до 9 знаков и до шага
// цены одним направленным режимом (покупка CEILING, продажа FLOOR), без
// двойного округления разными режимами. Проскальзывание равно 0, если κ = 0 или
// barVolumeLots <= 0 (нет объёма, от которого его считать). Цена продажи не
// опускается ниже одного шага цены (при q >> V множитель m мог бы стать
// отрицательным). lots <= 0 - std::invalid_argument.
Decimal fillPrice(Side Side, const Decimal &Base, std::int64_t Lots,
                  std::int64_t BarVolumeLots, const CostConfig &Costs,
                  const Decimal &PriceStep);

struct Amounts {
  Decimal Value, Commission, Fee;
};
// value = price·lots·lotSize до копейки HALF_UP; комиссия и сбор - от value,
// каждые до копейки CEILING.
Amounts amounts(const Decimal &Price, std::int64_t Lots, std::int64_t LotSize,
                const CostConfig &Costs);

// Разбивка разницы цены исполнения и базовой цены (для отчёта о сделке).
struct CostSplit {
  Decimal SpreadCost, SlippageCost;
};
// spreadCost = base·s/2·lots·lotSize до копейки HALF_UP; slippageCost = |price
// − base|·lots·lotSize до копейки HALF_UP минус spreadCost (может быть
// отрицательным из-за округления цены до шага).
CostSplit costSplit(const Decimal &Base, const Decimal &Price,
                    std::int64_t Lots, std::int64_t LotSize,
                    const CostConfig &Costs);

} // namespace core
