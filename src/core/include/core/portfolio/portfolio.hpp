#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "core/money/decimal.hpp"

// Портфель: деньги, позиции по бумагам и сделки «покупка -> продажа» (ФТ-10, ФТ-17, ФТ-20).
// Все суммы - Decimal (INV-6); порядок расчёта не зависит от платформы (INV-4).
// Проверки достаточности денег и позиции - в исполнении (задача 11), здесь только учёт.

namespace core {

enum class Side { Buy, Sell };

// Точка кривой стоимости портфеля: шаг шкалы времени сессии и стоимость портфеля в этот шаг.
struct EquityPoint {
  std::int64_t step;
  Decimal equity;
};

// Закрытая (полностью или частично) часть покупки: покупка и продажа сопоставляются по FIFO.
// Суммы - до копейки (HALF_UP), пропорционально сопоставленной части.
struct RoundTrip {
  int instrument;
  std::int64_t entryStep, exitStep, entryOrderId, lots;
  Decimal entryValue, exitValue, costs, netPnl;
};

class Portfolio {
 public:
  Portfolio(Decimal initialCash, std::size_t instrumentCount);

  const Decimal& cash() const { return cash_; }
  std::int64_t lots(int instrument) const;
  // Средневзвешенная цена покупки открытой позиции, 9 знаков HALF_UP; 0, если позиции нет.
  Decimal avgPrice(int instrument) const;

  // Учитывает исполнение. `value` - сумма сделки (до копейки), `price` - цена за единицу бумаги.
  // Покупка: cash -= value + commission + fee; продажа: cash += value - commission - fee.
  // Продажа больше позиции - std::logic_error, состояние не меняется.
  void applyFill(int instrument, Side side, std::int64_t lots, const Decimal& price, const Decimal& value,
                 const Decimal& commission, const Decimal& fee, std::int64_t step, std::int64_t orderId);

  // close * lots * lotSize до копейки HALF_UP (`decisions.md`, «Деньги и округление», п. 6).
  static Decimal positionValue(std::int64_t lots, std::int64_t lotSize, const Decimal& close);

  // Деньги + стоимости позиций по последним close. `lastClose[i]` пуст, если у бумаги ещё не было бара;
  // при открытой позиции это ошибка (std::logic_error). Размеры векторов - instrumentCount.
  Decimal equity(const std::vector<std::optional<Decimal>>& lastClose,
                 const std::vector<std::int64_t>& lotSizes) const;

  // Сопоставленные пары в порядке продаж, внутри продажи - в порядке FIFO покупок.
  const std::vector<RoundTrip>& roundTrips() const { return roundTrips_; }

 private:
  // Непогашенная часть покупки в очереди FIFO.
  struct OpenLot {
    std::int64_t step, orderId;
    std::int64_t lots, lotsLeft;
    Decimal value, costs;  // сумма сделки и комиссия + сбор всей покупки
  };
  struct Position {
    std::int64_t lots = 0;
    Decimal avgPrice;
    std::deque<OpenLot> open;
  };

  Position& position(int instrument);
  const Position& position(int instrument) const;

  Decimal cash_;
  std::vector<Position> positions_;
  std::vector<RoundTrip> roundTrips_;
};

}  // namespace core
