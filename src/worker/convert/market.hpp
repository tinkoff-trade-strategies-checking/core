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
#include "core/report/benchmark.hpp"
#include "handlers.hpp"

// Конвертеры рынка и данных: MarketSpec -> SessionSpec и Timeline, бар ->
// proto. Чистые функции: без состояния и часов (INV-4). Ошибки спецификации -
// std::invalid_argument.

namespace worker {

// Спецификация сессии из MarketSpec. Пустой список бумаг, повтор ключа, лот <=
// 0, шаг цены <= 0, капитал <= 0, неуказанные издержки - std::invalid_argument.
// Лимит ликвидности без поля liquidity_share - nullopt (без лимита).
core::SessionSpec toSessionSpec(const pb::MarketSpec &Market);

// Наибольший прогрев (MarketSpec.warmup_bars) - как лимит свечей ручной сессии
// (`decisions.md`).
inline constexpr std::int32_t kMaxWarmupBars = 5000;

// Шкала времени сессии по снапшоту и периоду; с прогревом warmup_bars (вне
// 0…kMaxWarmupBars - std::invalid_argument). Нет версии данных, нет бумаги или
// нет ни одного бара за период - core::SnapshotNotFound /
// core::SecurityNotFound (statusFromCurrentException переводит в
// FAILED_PRECONDITION).
std::shared_ptr<const core::Timeline>
loadTimeline(Context &Context, const pb::MarketSpec &Market);

// Бенчмарк: open первого дневного бара и close каждого дня бумаги
// benchmark_ticker за период [from_date; to_date] по D1. Нет тикера, нет бумаги
// в снапшоте или нет баров за период - nullopt.
std::optional<core::BenchmarkSeries>
loadBenchmark(Context &Context, const pb::MarketSpec &Market);

// "yyyy-mm-dd" -> yyyymmdd; неверная строка - std::invalid_argument.
std::int32_t parseIsoDate(const std::string &Text);
// yyyymmdd -> "yyyy-mm-dd".
std::string formatIsoDate(std::int32_t Yyyymmdd);

// Бар шага. instrument_key берётся из шкалы времени.
void toProto(const core::StepBar &Bar, const core::Timeline &Timeline,
             pb::Bar *Out);

// Бары шагов fromStep…toStep по возрастанию (step, бумага), включая бары
// прогрева (шаги < 0). Диапазон вне
// [-warmupSteps; totalSteps) - std::invalid_argument.
void appendBars(const core::Timeline &Timeline, std::int64_t FromStep,
                std::int64_t ToStep,
                google::protobuf::RepeatedPtrField<pb::Bar> *Out);

// Перевод исключения, пойманного в обработчике, в статус gRPC. Вызывать только
// из catch (...): ClickHouseError -> UNAVAILABLE; SnapshotNotFound,
// SecurityNotFound -> FAILED_PRECONDITION (контракт simulation.proto);
// IntervalUnavailable (интервал недоступен) -> NOT_FOUND; прочее
// MarketDataError (повреждённые данные) -> INTERNAL; std::invalid_argument ->
// INVALID_ARGUMENT; всё остальное -> INTERNAL без внутренних деталей.
grpc::Status statusFromCurrentException();

} // namespace worker
