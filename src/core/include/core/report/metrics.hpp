#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/dsl/diagnostic.hpp"
#include "core/exec/order.hpp"  // Fill
#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp"  // EquityPoint, RoundTrip

// Метрики отчёта (ФТ-17, НФТ-4). Деньги остаются Decimal (INV-6); метрики - double и считаются только здесь:
// Decimal переводится в double через toDouble(), порядок вычислений фиксирован (INV-4), допуск 1e-6 (`decisions.md`,
// «Деньги и округление», п. 9-11). Безрисковая ставка 0, 252 торговых дня в году, стандартное отклонение выборочное.

namespace core {

// Вход расчёта. Вектора по шагам индексируются номером шага шкалы времени сессии (шаг 0, 1, ...).
struct MetricsInput {
  Decimal initialCash;                         // начальный капитал E0 > 0
  std::vector<EquityPoint> equity;             // стоимость портфеля по шагам, шаги строго по возрастанию
  std::vector<std::int32_t> tradingDayOfStep;  // номер торгового дня шага; размер > последнего шага equity
  std::vector<Fill> fills;
  std::vector<RoundTrip> roundTrips;
  std::int64_t rejections = 0, partialFills = 0;
  // [шаг][бумага]: стоимость позиции (>= 0, 0 без позиции) после обработки шага. Пусто - доли позиций не считаются.
  std::vector<std::vector<Decimal>> positionValuesByStep;
  std::optional<std::pair<Decimal, Decimal>> benchmark;  // (open первого бара, close последнего бара) IMOEX
};

struct Metrics {
  double totalReturn = 0, grossReturn = 0;
  std::optional<double> cagr;  // нет при нуле торговых дней
  double maxDrawdown = 0;      // доля от пика, 0..1
  // Наибольшая серия подряд идущих точек equity ниже пика; точка equity - один шаг, поэтому это шаги.
  std::int64_t maxDrawdownSteps = 0;
  std::optional<double> volatility, sharpe, sortino;  // годовые; нет при менее чем двух днях и нулевом разбросе
  double turnover = 0;
  std::int64_t fills = 0, roundTrips = 0;
  std::optional<double> winRate;  // нет без закрытых пар
  std::int64_t rejections = 0, partialFills = 0;
  double hhiAvg = 0, maxPositionShare = 0;
  std::optional<double> benchmarkReturn;
  std::int32_t tradingDays = 0;
};

// Суммы издержек по всем сделкам. Не равны разности денег и net-результата пар на копейки:
// у каждой стороны сделки свои округления (`decisions.md`, «Деньги и округление»).
struct CostTotals {
  Decimal commission, fee, spread, slippage;
};

struct Report {
  Metrics metrics;
  CostTotals costs;
  std::vector<EquityPoint> equity;
  Decimal finalEquity;  // стоимость на последнем шаге; без точек - начальный капитал
};

// Находка проверок прогона (ФТ-14): код, уровень, сообщение, шаг.
struct Finding {
  std::string code;
  dsl::Level level = dsl::Level::Warning;
  std::string message;
  std::int64_t step = 0;
};

// Факторы цепочки «наивный -> честный» (ФТ-24); цепочку считает задача 27.
enum class CostFactor { Naive, Delay, Fees, Spread, Slippage, Liquidity };
// Флаги ФТ-24: издержки > 50 % прибыли, смена знака результата, Sharpe честного прогона > 3.
enum class CostFlag { CostsOverHalfProfit, SignChange, SharpeOver3 };

struct CostBreakdownStep {
  CostFactor factor;
  Decimal netResult;
  double netReturn;
};
struct CostBreakdown {
  std::vector<CostBreakdownStep> steps;
  std::vector<CostFlag> flags;
};

// Считает отчёт. std::invalid_argument: initialCash <= 0, шаг точки вне tradingDayOfStep, шаги не по возрастанию,
// строка positionValuesByStep короче шага точки.
Report computeReport(const MetricsInput& input);

}  // namespace core
