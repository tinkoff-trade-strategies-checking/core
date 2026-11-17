#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/exec/order.hpp"

// Журнал сессии: «номер шага + команда» (INV-8, ФТ-11). Команда из записи с
// step = k действует с шага k + 1 (INV-2). id заявки = seq · 2 + нога (нога 1 -
// вторая заявка OCO).

namespace core {

// Подача заявки; ocoWith - вторая нога OCO (пара «одна исполнилась - вторая
// снята»).
struct PlaceCmd {
  OrderSpec Order;
  std::optional<OrderSpec> OcoWith;
};

struct CancelCmd {
  std::int64_t OrderId = 0;
  Reason ReasonCode = Reason::CancelledByUser;
};

// Замена: проверяется новая заявка, и только если она прошла, старая (у OCO -
// вся пара) снимается.
struct ReplaceCmd {
  std::int64_t OrderId = 0;
  PlaceCmd Replacement;
};

struct JournalEntry {
  std::int64_t Seq = 0; // номер записи журнала с 0, строго возрастает
  std::int64_t Step = 0; // шаг, на котором дана команда; не убывает (несколько
                         // заявок на шаге - норма)
  std::variant<PlaceCmd, CancelCmd, ReplaceCmd> Command;
};

// Итог команды. Отклонённая подача и замена оставляют заявки в статусе Rejected
// (их id - в orderIds), чтобы отказ остался в журнале; отклонённая отмена
// заявок не создаёт.
struct CommandResult {
  std::int64_t Seq = 0;
  bool Accepted = false;
  Reason ReasonCode = Reason::None;
  std::string Field; // при InvalidParams: поле, из-за которого отказ
  std::vector<std::int64_t> OrderIds;
};

} // namespace core
