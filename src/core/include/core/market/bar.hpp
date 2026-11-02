#pragma once

#include <cstdint>
#include <string>

#include "core/money/decimal.hpp"

// Рыночные данные ядра (ФТ-3, ФТ-5). Деньги - Decimal (INV-6), время - миллисекунды Unix UTC
// (INV-4: никаких системных часов, только значения из данных).

namespace core {

// Один бар (свеча) серии. Цены - Decimal, не double (INV-6).
struct Bar {
  std::int64_t index = 0;          // номер в запрошенной серии, с 0, без пропусков
  std::int64_t tsOpenMs = 0;       // начало бара
  std::int64_t availableAtMs = 0;  // момент, когда бар стал известен (ФТ-5; не раньше закрытия)
  Decimal open;
  Decimal high;
  Decimal low;
  Decimal close;
  std::int64_t volume = 0;  // штук (не лотов)
  Decimal value;            // оборот в валюте инструмента

  friend bool operator==(const Bar&, const Bar&) = default;
};

// Справочные данные инструмента в конкретной версии данных (снапшоте).
struct Security {
  std::string secid;
  std::string sector;  // "" - не классифицирован
  std::int64_t lotSize = 1;
  Decimal priceStep;

  friend bool operator==(const Security&, const Security&) = default;
};

// Запрос серии баров. toMs - момент, к которому бар уже должен быть известен (INV-1): берутся бары
// с fromMs <= tsOpenMs < toMs И availableAtMs <= toMs, по возрастанию tsOpenMs; index нумеруется
// с 0 внутри результата.
struct BarRequest {
  std::string snapshotId;
  std::string secid;
  std::int32_t intervalSec = 0;
  std::int64_t fromMs = 0;
  std::int64_t toMs = 0;
};

}  // namespace core
