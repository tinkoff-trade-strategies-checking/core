#pragma once

#include "core/money/decimal.hpp"

// Мост double -> core::Decimal для цен заявок из DSL (`dsl.md`, INV-6).

namespace core {

// Преобразует конечную цену `RawPrice` in (0, 1e15] в `Decimal` через кратчайшее
// точное десятичное представление (`std::to_chars`) и одношаговое округление
// `HALF_UP` к `PriceStep` (> 0). При подшаговом обнулении возвращает `Decimal::zero()`,
// чтобы валидация ядра отклонила заявку с `InvalidParams`.
Decimal roundDslPriceToStep(double RawPrice, const Decimal &PriceStep);

} // namespace core
