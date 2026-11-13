#pragma once

#include "core/engine/journal.hpp"
#include "core/engine/session_engine.hpp"
#include "core/exec/order.hpp"
#include "core/market/timeline.hpp"
#include "handlers.hpp"

// Конвертеры состояния сессии: журнал proto -> ядро, заявки, сделки, портфель, события шагов -> proto.
// Ключ бумаги в proto (instrument_key) <-> индекс в SessionSpec::instruments. Чистые функции (INV-4).

namespace worker {

// Запись журнала. Неизвестный ключ бумаги, незаданная команда или перечисление - std::invalid_argument.
// Необязательные цены (limit_price, stop_price, trailing_percent) берутся только при наличии поля;
// oco_with с lots = 0 - заявка без OCO.
// rule_name и because переносятся как есть; причина снятия не задана - отмена пользователем.
core::JournalEntry fromProto(const pb::JournalEntry& entry, const core::SessionSpec& spec);
// Запись журнала ядра (в том числе стратегии) -> proto; fromProto(toProto(e)) == e.
void toProto(const core::JournalEntry& entry, const core::Timeline& timeline, pb::JournalEntry* out);

// Заявка. ocoSiblingId и closedStep «нет» остаются -1, как в ядре (id 0 - настоящая заявка).
void toProto(const core::Order& order, const core::Timeline& timeline, pb::Order* out);
void toProto(const core::Fill& fill, const core::Timeline& timeline, pb::Fill* out);
// В positions - только открытые позиции (lots != 0), по порядку бумаг сессии.
void toProto(const core::PortfolioSnapshot& snapshot, const core::Timeline& timeline, pb::Portfolio* out);
void toProto(const core::CommandResult& result, pb::CommandResult* out);
void toProto(const core::StepEvents& events, const core::Timeline& timeline, pb::StepEvents* out);

pb::SessionStatus toProto(core::SessionStatus status);
pb::Side toProto(core::Side side);
pb::OrderType toProto(core::OrderType type);
pb::TimeInForce toProto(core::TimeInForce tif);
pb::OrderStatus toProto(core::OrderStatus status);
pb::Reason toProto(core::Reason reason);
// Обратно к toProto(Reason); неизвестное значение - std::invalid_argument.
core::Reason reasonFromProto(pb::Reason reason);

}  // namespace worker
