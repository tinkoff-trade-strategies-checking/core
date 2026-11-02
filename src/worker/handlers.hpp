#pragma once

#include <grpcpp/grpcpp.h>

#include "core/market/bar_source.hpp"
#include "core/market/calendar_source.hpp"
#include "simulation.grpc.pb.h"

namespace worker {

namespace pb = pac::simulation::v1;

// Зависимости обработчиков: источники данных живут дольше сервиса (создаёт main или тест).
// Обработчики не владеют ими и не меняют; чтение баров и календаря - только через эти интерфейсы.
struct Context {
  core::BarSource& bars;
  core::CalendarSource& calendar;
};

// Обработчики RPC. Каждый живёт в своём handlers/<имя>.cpp; WorkerService только передаёт им
// запрос. Инварианты для всех обработчиков:
//  - INV-1: наружу не уходят бары позже запрошенных up_to_step / курсора;
//  - INV-7: в слепом режиме нет реальных дат и границ торговых дней.
// Не реализованы (UNIMPLEMENTED): RunBacktest, RunWalkForward, AnalyzeDivergences.
grpc::Status handleCheckStrategy(Context& context, const pb::CheckStrategyRequest& request,
                                 pb::CheckStrategyResponse* response);
grpc::Status handleReplaySession(Context& context, const pb::ReplaySessionRequest& request,
                                 pb::ReplaySessionResponse* response);
grpc::Status handleGetBars(Context& context, const pb::GetBarsRequest& request,
                           pb::GetBarsResponse* response);
grpc::Status handleRunBacktest(Context& context, const pb::RunBacktestRequest& request,
                               grpc::ServerWriterInterface<pb::RunBacktestUpdate>* writer);
grpc::Status handleRunWalkForward(Context& context, const pb::WalkForwardRequest& request,
                                  grpc::ServerWriterInterface<pb::WalkForwardUpdate>* writer);
grpc::Status handleAnalyzeDivergences(Context& context, const pb::DivergenceRequest& request,
                                      pb::DivergenceResponse* response);

}  // namespace worker
