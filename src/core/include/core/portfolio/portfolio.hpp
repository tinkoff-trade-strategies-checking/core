#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "core/money/decimal.hpp"

// Портфель: деньги, позиции по бумагам и сделки «покупка -> продажа» (ФТ-10,
// ФТ-17). Все суммы - Decimal (INV-6); порядок расчёта не зависит от
// платформы (INV-4). Проверки достаточности денег и позиции - в исполнении
// (задача 11), здесь только учёт.

namespace core {

enum class Side { Buy, Sell };

// Точка кривой стоимости портфеля: шаг шкалы времени сессии и стоимость
// портфеля в этот шаг.
struct EquityPoint {
  std::int64_t Step;
  Decimal Equity;
};

// Закрытая (полностью или частично) часть покупки: покупка и продажа
// сопоставляются по FIFO. Суммы - до копейки (HALF_UP), пропорционально
// сопоставленной части.
struct RoundTrip {
  int Instrument;
  std::int64_t EntryStep, ExitStep, EntryOrderId, Lots;
  Decimal EntryPrice, ExitPrice; // цена за единицу бумаги входа и выхода
  Decimal EntryValue, ExitValue, Costs, NetPnl;
  bool IsShort = false; // true для пары «шорт (продажа) -> выкуп (покупка)»

  Decimal grossPnl() const {
    return IsShort ? (EntryValue - ExitValue) : (ExitValue - EntryValue);
  }
};

class Portfolio {
public:
  Portfolio(Decimal InitialCash, std::size_t InstrumentCount,
            bool AllowShort = false);

  const Decimal &cash() const { return Cash; }
  bool allowShort() const { return AllowShort; }
  std::int64_t lots(int Instrument) const;
  // Средневзвешенная цена входа открытой позиции, 9 знаков HALF_UP; 0, если
  // позиции нет.
  Decimal avgPrice(int Instrument) const;
  // Себестоимость открытой позиции: точная сумма входа непогашенных лотов
  // (без комиссии и сбора), по лотам FIFO пропорционально остатку до копейки
  // HALF_UP, как EntryValue пар; для короткой позиции - со знаком минус; 0 без
  // позиции.
  Decimal costBasis(int Instrument) const;

  // Учитывает исполнение. `value` - сумма сделки (до копейки), `price` - цена
  // за единицу бумаги. Покупка: cash -= value + commission + fee; продажа: cash
  // += value - commission - fee. При AllowShort == false продажа больше позиции
  // - std::logic_error, состояние не меняется.
  void applyFill(int Instrument, Side Side, std::int64_t Lots,
                 const Decimal &Price, const Decimal &Value,
                 const Decimal &Commission, const Decimal &Fee,
                 std::int64_t Step, std::int64_t OrderId);

  // Списывает комиссию/плату за перенос овернайт (short borrow / margin loan)
  // из денежных средств. Amount < 0 - std::invalid_argument.
  void chargeFee(const Decimal &Amount);

  // close * lots * lotSize до копейки HALF_UP (`decisions.md`, «Деньги и
  // округление», п. 6).
  static Decimal positionValue(std::int64_t Lots, std::int64_t LotSize,
                               const Decimal &Close);

  // Деньги + стоимости позиций по последним close. `lastClose[i]` пуст, если у
  // бумаги ещё не было бара; при открытой позиции это ошибка
  // (std::logic_error). Размеры векторов - instrumentCount.
  Decimal equity(const std::vector<std::optional<Decimal>> &LastClose,
                 const std::vector<std::int64_t> &LotSizes) const;

  // Сопоставленные пары в порядке продаж, внутри продажи - в порядке FIFO
  // покупок.
  const std::vector<RoundTrip> &roundTrips() const { return RoundTrips; }

private:
  // Непогашенная часть покупки в очереди FIFO.
  struct OpenLot {
    std::int64_t Step, OrderId;
    std::int64_t Lots, LotsLeft;
    Decimal Price, Value,
        Costs; // цена покупки, сумма сделки и комиссия + сбор всей покупки
  };
  struct Position {
    std::int64_t Lots = 0;
    Decimal AvgPrice;
    std::deque<OpenLot> Open;
  };

  Position &position(int Instrument);
  const Position &position(int Instrument) const;

  Decimal Cash;
  bool AllowShort = false;
  std::vector<Position> Positions;
  std::vector<RoundTrip> RoundTrips;
};

} // namespace core
