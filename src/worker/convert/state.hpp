#pragma once

#include "core/engine/journal.hpp"
#include "core/engine/session_engine.hpp"
#include "core/exec/order.hpp"
#include "core/market/timeline.hpp"
#include "handlers.hpp"

// Конвертеры состояния сессии: журнал proto -> ядро, заявки, сделки, портфель,
// события шагов -> proto. Ключ бумаги в proto (instrument_key) <-> индекс в
// SessionSpec::instruments. Чистые функции (INV-4).

namespace worker {

// Запись журнала. Неизвестный ключ бумаги, незаданная команда или перечисление
// - std::invalid_argument. Необязательные цены (limit_price, stop_price,
// trailing_percent) берутся только при наличии поля; oco_with с lots = 0 -
// заявка без OCO. rule_name и because переносятся как есть; причина снятия не
// задана - отмена пользователем.
core::JournalEntry fromProto(const pb::JournalEntry &Entry,
                             const core::SessionSpec &Spec);
// Запись журнала ядра (в том числе стратегии) -> proto; fromProto(toProto(e))
// == e.
void toProto(const core::JournalEntry &Entry, const core::Timeline &Timeline,
             pb::JournalEntry *Out);

// Заявка. ocoSiblingId и closedStep «нет» остаются -1, как в ядре (id 0 -
// настоящая заявка).
void toProto(const core::Order &Order, const core::Timeline &Timeline,
             pb::Order *Out);
void toProto(const core::Fill &Fill, const core::Timeline &Timeline,
             pb::Fill *Out);
// В positions - только открытые позиции (lots != 0), по порядку бумаг сессии.
void toProto(const core::PortfolioSnapshot &Snapshot,
             const core::Timeline &Timeline, pb::Portfolio *Out);
void toProto(const core::CommandResult &Result, pb::CommandResult *Out);
void toProto(const core::StepEvents &Events, const core::Timeline &Timeline,
             pb::StepEvents *Out);

pb::SessionStatus toProto(core::SessionStatus Status);
pb::Side toProto(core::Side Side);
pb::OrderType toProto(core::OrderType Type);
pb::TimeInForce toProto(core::TimeInForce Tif);
pb::OrderStatus toProto(core::OrderStatus Status);
pb::Reason toProto(core::Reason Reason);
// Обратно к toProto(Reason); неизвестное значение - std::invalid_argument.
core::Reason reasonFromProto(pb::Reason Reason);

} // namespace worker
