#pragma once

#include "core/money/decimal.hpp"

// Перевод Decimal <-> сообщение `pac.simulation.v1.Decimal{int64 units; int32
// nano}` (proto/common.proto). Шаблоны над сгенерированным типом: ядро не
// зависит от protobuf, а заголовок включают только worker и тесты, которые уже
// линкуются с simulation_proto. Знаки и диапазон проверяет
// Decimal::fromUnitsNano (ADR-0008).

namespace core {

template <class ProtoDecimal> Decimal fromProto(const ProtoDecimal &msg) {
  return Decimal::fromUnitsNano(msg.units(), msg.nano());
}

template <class ProtoDecimal>
void toProto(const Decimal &value, ProtoDecimal *out) {
  const UnitsNano parts = value.toUnitsNano();
  out->set_units(parts.units);
  out->set_nano(parts.nano);
}

template <class ProtoDecimal> ProtoDecimal toProto(const Decimal &value) {
  ProtoDecimal msg;
  toProto(value, &msg);
  return msg;
}

} // namespace core
