#pragma once

#include <chrono>
#include <functional>
#include <optional>

#include <grpcpp/grpcpp.h>

#include "core/market/bar_source.hpp"
#include "core/market/calendar_source.hpp"
#include "core/market/ruonia_source.hpp"
#include "simulation.grpc.pb.h"

namespace worker {

namespace pb = pac::simulation::v1;

class TimelineCache;

// Зависимости обработчиков: источники данных живут дольше сервиса (создаёт main
// или тест). Обработчики не владеют ими и не меняют; чтение баров, календаря и
// ставок RUONIA - только через эти интерфейсы. Кэш шкалы опционален (в тестах
// может быть nullptr).
struct Context {
  core::BarSource &Bars;
  core::CalendarSource &Calendar;
  core::RuoniaSource &Ruonia;
  TimelineCache *Cache = nullptr;
};

// Обработчики RPC. Каждый живёт в своём handlers/<имя>.cpp; WorkerService
// только передаёт им запрос. Инварианты для всех обработчиков:
//  - INV-1: наружу не уходят бары позже запрошенных up_to_step / курсора;
//  - INV-7: в слепом режиме нет реальных дат и границ торговых дней.
grpc::Status handleReplaySession(Context &Context,
                                 const pb::ReplaySessionRequest &Request,
                                 pb::ReplaySessionResponse *Response);
grpc::Status handleGetBars(Context &Context, const pb::GetBarsRequest &Request,
                           pb::GetBarsResponse *Response);
grpc::Status handleEstimateOrder(Context &Context,
                                 const pb::EstimateOrderRequest &Request,
                                 pb::EstimateOrderResponse *Response);
grpc::Status handleClassifyPeriods(Context &Context,
                                   const pb::ClassifyPeriodsRequest &Request,
                                   pb::ClassifyPeriodsResponse *Response);
grpc::Status handleCheckStrategy(const pb::CheckStrategyRequest &Request,
                                 pb::CheckStrategyResponse *Response);
// Максимальный бюджет времени прогона стратегии и разбора расхождений (НФТ-7).
inline constexpr std::chrono::steady_clock::duration kMaxRunTimeBudget =
    std::chrono::minutes(10);

inline constexpr const char *kTimeBudgetExceededMessage =
    "превышен бюджет времени прогона (НФТ-7)";

// Вычисляет бюджет времени прогона из дедлайна gRPC-контекста: если дедлайн
// задан и не равен system_clock::time_point::max(), возвращает
// min(kMaxRunTimeBudget, max(0, Deadline - now)), иначе kMaxRunTimeBudget.
std::chrono::steady_clock::duration
computeTimeBudget(std::chrono::system_clock::time_point Deadline);

grpc::Status handleRunStrategy(
    Context &Ctx, const pb::RunStrategyRequest &Request,
    grpc::ServerWriter<pb::RunStrategyUpdate> *Writer,
    const std::function<bool()> &IsCancelled = {},
    std::optional<std::chrono::steady_clock::duration> TimeBudget =
        kMaxRunTimeBudget);
grpc::Status handleRunStrategyWithSink(
    Context &Ctx, const pb::RunStrategyRequest &Request,
    const std::function<bool(const pb::RunStrategyUpdate &)> &WriteUpdate,
    const std::function<bool()> &IsCancelled = {},
    std::optional<std::chrono::steady_clock::duration> TimeBudget =
        kMaxRunTimeBudget);
grpc::Status handleAnalyzeDivergences(
    Context &Ctx, const pb::DivergenceRequest &Request,
    pb::DivergenceResponse *Response,
    const std::function<bool()> &IsCancelled = {},
    std::optional<std::chrono::steady_clock::duration> TimeBudget =
        kMaxRunTimeBudget);
} // namespace worker
