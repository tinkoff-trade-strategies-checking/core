#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/dsl/ast.hpp"
#include "core/dsl/checker.hpp"
#include "core/exec/order.hpp"
#include "core/market/step_bar.hpp"
#include "core/money/decimal.hpp"

// Интерпретатор правил DSL (dsl.md, «Как исполняется», «Действия», «Причина»).
// Выражения считаются в double (`na` - nullopt), деньги появляются только при
// создании заявки и считаются в Decimal (INV-6). Стратегия видит только бары,
// переданные в onBar, то есть не дальше курсора (INV-1), и ничего не знает о
// новостях (INV-9). Без часов и случайности (INV-4).
//
// Не здесь, а в прогоне стратегии (задача 21): книга «одна активная заявка на
// правило» и снятие продаж при нулевой позиции.

namespace core::dsl {

// Переопределения параметров на запуск; остальные параметры - значения по
// умолчанию.
using ParamValues = std::map<std::string, double>;

// Состояние портфеля на закрытии бара (в разборе расхождений - портфель
// человека). positionLots >= 0; entryPrice и barsInPosition - nullopt, если
// позиции нет.
struct StateView {
  std::int64_t PositionLots = 0;
  std::optional<double> EntryPrice;
  double Cash = 0, Equity = 0;
  std::optional<std::int64_t> BarsInPosition;
};

// Намерение: что правило хочет сделать на этом баре. cancelAll - `cancel`
// (order не заполнен, кроме instrument); иначе order - готовая заявка: размер в
// лотах >= 1, цены в Decimal и кратны шагу цены. because - текст причины с
// подставленными значениями (ФТ-12): он уходит в заявку.
struct Intent {
  int Instrument = 0;
  std::string RuleName, Because;
  bool CancelAll = false;
  OrderSpec Order;
};

// Экземпляр стратегии на один прогон: своя копия программы с выбранными
// значениями параметров, индикаторы и ряды по каждой бумаге. Не копируется.
class StrategyInstance {
public:
  // params - результат check() исходной программы. overrides записываются в
  // КОПИЮ программы (ParamDecl::value), и check() выполняется заново: любая
  // ошибка отказывает в создании экземпляра (INV-1). Иначе `k = -1` для
  // `close[k]` читало бы будущее, а `n = 0` или `n = 100000` для `sma(close,
  // n)` ломало бы индикатор. Значение вне диапазона параметра, неизвестный
  // параметр, не конечное число - тоже отказ. Все отказы -
  // std::invalid_argument с русским сообщением. instrumentCount >= 1: число
  // бумаг сессии.
  StrategyInstance(const Program &Program,
                   const std::vector<ResolvedParam> &Params,
                   const ParamValues &Overrides, std::size_t InstrumentCount);
  ~StrategyInstance();
  StrategyInstance(StrategyInstance &&) noexcept;
  StrategyInstance &operator=(StrategyInstance &&) noexcept;
  StrategyInstance(const StrategyInstance &) = delete;
  StrategyInstance &operator=(const StrategyInstance &) = delete;

  // Новый бар бумаги: обновить ряды и индикаторы, которые не зависят от
  // состояния портфеля. Вызывается на каждом баре бумаги, по возрастанию step,
  // до evaluate этого бара: индикаторы обновляются независимо от того, дошло ли
  // вычисление до них в условии (`and` / `or`). bar.instrument должен совпадать
  // с instrument, step строго возрастает по бумаге - иначе
  // std::invalid_argument. Предыдущий бар этой бумаги должен быть вычислен
  // через evaluate, иначе std::logic_error.
  void onBar(int Instrument, const StepBar &Bar);

  // Правила в порядке текста на закрытии бара, уже переданного в onBar.
  // Состояние для всех правил одно; сработавшее правило даёт не больше одного
  // намерения. Индикаторы, зависящие от состояния портфеля (`sma(position,
  // 3)`), обновляются здесь - значит, evaluate нужно вызывать на КАЖДОМ баре,
  // переданном в onBar, иначе такие индикаторы потеряют отсчёт. Бар не тот, что
  // в последнем onBar, или повторный вызов для того же бара - std::logic_error.
  // Действие, у которого количество меньше 1 лота, количество либо цена `na`,
  // цена <= 0 (после округления до шага тоже) - намерения не даёт.
  std::vector<Intent> evaluate(int Instrument, const StepBar &Bar,
                               std::int64_t LotSize, const Decimal &PriceStep,
                               const StateView &State);

private:
  struct Impl;
  std::unique_ptr<Impl> Pimpl;
};

// Цена из выражения -> Decimal: to_chars(fixed, 9) -> Decimal::parse ->
// roundToStep(HALF_UP) (decisions.md, «Деньги и округление»). Не конечное число
// - std::invalid_argument, не помещается - DecimalOverflow. priceStep > 0.
Decimal toDecimalPrice(double Price, const Decimal &PriceStep);

} // namespace core::dsl
