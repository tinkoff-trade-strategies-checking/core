#pragma once

#include "core/money/decimal.hpp"

// Перевод Decimal <-> сообщение `pac.simulation.v1.Decimal{int64 units; int32
// nano}` (proto/common.proto). Шаблоны над сгенерированным типом: ядро не
// зависит от protobuf, а заголовок включают только worker и тесты, которые уже
// линкуются с simulation_proto. Знаки и диапазон проверяет
// Decimal::fromUnitsNano (`decisions.md`, «Деньги в C++»).

namespace core {

template <class ProtoDecimal> Decimal fromProto(const ProtoDecimal &Msg) {
  return Decimal::fromUnitsNano(Msg.units(), Msg.nano());
}

template <class ProtoDecimal>
void toProto(const Decimal &Value, ProtoDecimal *Out) {
  const UnitsNano Parts = Value.toUnitsNano();
  Out->set_units(Parts.Units);
  Out->set_nano(Parts.Nano);
}

template <class ProtoDecimal> ProtoDecimal toProto(const Decimal &Value) {
  ProtoDecimal Msg;
  toProto(Value, &Msg);
  return Msg;
}

} // namespace core
