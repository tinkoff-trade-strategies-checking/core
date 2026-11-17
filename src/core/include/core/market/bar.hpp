#pragma once

#include <cstdint>
#include <string>

#include "core/money/decimal.hpp"

// Рыночные данные ядра (ФТ-3, ФТ-5). Деньги - Decimal (INV-6), время -
// миллисекунды Unix UTC (INV-4: никаких системных часов, только значения из
// данных).

namespace core {

// Один бар (свеча) серии. Цены - Decimal, не double (INV-6).
struct Bar {
  std::int64_t Index = 0; // номер в запрошенной серии, с 0, без пропусков
  std::int64_t TsOpenMs = 0; // начало бара
  std::int64_t AvailableAtMs =
      0; // момент, когда бар стал известен (ФТ-5; не раньше закрытия)
  Decimal Open;
  Decimal High;
  Decimal Low;
  Decimal Close;
  std::int64_t Volume = 0; // штук (не лотов)
  Decimal Value; // оборот в валюте инструмента

  friend bool operator==(const Bar &, const Bar &) = default;
};

// Справочные данные инструмента в конкретной версии данных (снапшоте).
struct Security {
  std::string Secid;
  std::string Sector; // "" - не классифицирован
  std::int64_t LotSize = 1;
  Decimal PriceStep;

  friend bool operator==(const Security &, const Security &) = default;
};

// Запрос серии баров. toMs - момент, к которому бар уже должен быть известен
// (INV-1): берутся бары с fromMs <= tsOpenMs < toMs И availableAtMs <= toMs, по
// возрастанию tsOpenMs; index нумеруется с 0 внутри результата.
struct BarRequest {
  std::string SnapshotId;
  std::string Secid;
  std::int32_t IntervalSec = 0;
  std::int64_t FromMs = 0;
  std::int64_t ToMs = 0;
};

} // namespace core
