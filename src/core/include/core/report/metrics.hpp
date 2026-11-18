#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/exec/order.hpp" // Fill
#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp" // EquityPoint, RoundTrip

// Метрики отчёта (ФТ-17, НФТ-4). Деньги остаются Decimal (INV-6); метрики -
// double и считаются только здесь: Decimal переводится в double через
// toDouble(), порядок вычислений фиксирован (INV-4), допуск 1e-6
// (`decisions.md`, «Деньги и округление», п. 9-11). Безрисковая ставка - дневной
// ряд RUONIA (% годовых, Actual/365, `decisions.md`, «Метрики отчёта»), 252
// торговых дня в году, стандартное отклонение выборочное.

namespace core {

// Вход расчёта. Вектора по шагам индексируются номером шага шкалы времени
// сессии (шаг 0, 1, ...).
struct MetricsInput {
  Decimal InitialCash; // начальный капитал E0 > 0
  std::vector<EquityPoint>
      Equity; // стоимость портфеля по шагам, шаги строго по возрастанию
  std::vector<std::int32_t> TradingDayOfStep; // номер торгового дня шага;
                                              // размер > последнего шага equity
  std::vector<Fill> Fills;
  std::vector<RoundTrip> RoundTrips;
  std::int64_t Rejections = 0, PartialFills = 0;
  // [шаг][бумага]: стоимость позиции (>= 0, 0 без позиции) после обработки
  // шага. Пусто - доли позиций не считаются.
  std::vector<std::vector<Decimal>> PositionValuesByStep;
  std::optional<std::pair<Decimal, Decimal>>
      Benchmark; // (open первого бара, close последнего бара) IMOEX
  std::vector<EquityPoint>
      BenchmarkEquity; // кривая бенчмарка в рублях капитала
                       // (benchmarkEquity()), в отчёт как есть
  // Дневная ставка RUONIA (% годовых) на каждый торговый день (размер равен
  // числу торговых дней). При tradingDays >= 2 отсутствие - MarketDataError.
  std::vector<Decimal> RuoniaRates;
};

struct Metrics {
  double TotalReturn = 0, GrossReturn = 0;
  std::optional<double> Cagr; // нет при нуле торговых дней
  double MaxDrawdown = 0;     // доля от пика, 0..1
  // Наибольшая серия подряд идущих точек equity ниже пика; точка equity - один
  // шаг, поэтому это шаги.
  std::int64_t MaxDrawdownSteps = 0;
  std::optional<double> Volatility, Sharpe,
      Sortino; // годовые; нет при менее чем двух днях и нулевом разбросе
  double Turnover = 0;
  std::int64_t Fills = 0, RoundTrips = 0;
  std::optional<double> WinRate; // нет без закрытых пар
  std::int64_t Rejections = 0, PartialFills = 0;
  double HhiAvg = 0, MaxPositionShare = 0;
  std::optional<double> BenchmarkReturn;
  std::int32_t TradingDays = 0;
  std::int64_t StopTakeSameBar = 0; // сделок, где стоп и тейк задеты одной
                                    // свечой (Fill::stopAndTakeSameBar)
};

// Суммы издержек по всем сделкам. Не равны разности денег и net-результата пар
// на копейки: у каждой стороны сделки свои округления (`decisions.md`, «Деньги
// и округление»).
struct CostTotals {
  Decimal Commission, Fee, Spread, Slippage;
};

struct Report {
  Metrics Stats;
  CostTotals Costs;
  std::vector<EquityPoint> Equity;
  Decimal
      FinalEquity; // стоимость на последнем шаге; без точек - начальный капитал
  std::vector<EquityPoint> BenchmarkEquity; // из MetricsInput::benchmarkEquity
};

// Считает отчёт. std::invalid_argument: initialCash <= 0, шаг точки вне
// tradingDayOfStep, шаги не по возрастанию, строка positionValuesByStep короче
// шага точки.
Report computeReport(const MetricsInput &Input);

} // namespace core
