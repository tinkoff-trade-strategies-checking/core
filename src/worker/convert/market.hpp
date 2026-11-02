#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "core/engine/session_engine.hpp"
#include "core/market/step_bar.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "handlers.hpp"

// Конвертеры рынка и данных: MarketSpec -> SessionSpec и Timeline, бар -> proto. Чистые функции: без состояния и
// часов (INV-4). Ошибки спецификации - std::invalid_argument.

namespace worker {

// Спецификация сессии из MarketSpec. Пустой список бумаг, повтор ключа, лот <= 0, шаг цены <= 0, капитал <= 0,
// неуказанные издержки (кроме naive: там движок берёт нулевые сам) - std::invalid_argument.
// Лимит ликвидности без поля liquidity_share - nullopt (без лимита).
core::SessionSpec toSessionSpec(const pb::MarketSpec& market);

// Шкала времени сессии по снапшоту и периоду. Нет версии данных, нет бумаги или нет ни одного бара за период -
// core::SnapshotNotFound / core::SecurityNotFound (statusFromCurrentException переводит в FAILED_PRECONDITION).
std::shared_ptr<const core::Timeline> loadTimeline(Context& context, const pb::MarketSpec& market);

// Бенчмарк: (open первого дневного бара, close последнего) бумаги benchmark_ticker за период [from_date; to_date] по D1.
// Нет тикера, нет бумаги в снапшоте или нет баров за период - nullopt.
std::optional<std::pair<core::Decimal, core::Decimal>> loadBenchmark(Context& context, const pb::MarketSpec& market);

// "yyyy-mm-dd" -> yyyymmdd; неверная строка - std::invalid_argument.
std::int32_t parseIsoDate(const std::string& text);
// yyyymmdd -> "yyyy-mm-dd".
std::string formatIsoDate(std::int32_t yyyymmdd);

// Бар шага. instrument_key берётся из шкалы времени.
void toProto(const core::StepBar& bar, const core::Timeline& timeline, pb::Bar* out);

// Перевод исключения, пойманного в обработчике, в статус gRPC. Вызывать только из catch (...):
// ClickHouseError -> UNAVAILABLE; SnapshotNotFound, SecurityNotFound -> FAILED_PRECONDITION (контракт simulation.proto);
// IntervalUnavailable (интервал недоступен) -> NOT_FOUND; прочее MarketDataError (повреждённые данные) -> INTERNAL;
// std::invalid_argument -> INVALID_ARGUMENT; всё остальное -> INTERNAL
// без внутренних деталей.
grpc::Status statusFromCurrentException();

}  // namespace worker
