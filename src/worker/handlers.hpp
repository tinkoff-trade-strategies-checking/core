#pragma once

#include <grpcpp/grpcpp.h>

#include "core/market/bar_source.hpp"
#include "core/market/calendar_source.hpp"
#include "core/market/ruonia_source.hpp"
#include "simulation.grpc.pb.h"

namespace worker {

namespace pb = pac::simulation::v1;

// Зависимости обработчиков: источники данных живут дольше сервиса (создаёт main
// или тест). Обработчики не владеют ими и не меняют; чтение баров, календаря и
// ставок RUONIA - только через эти интерфейсы.
struct Context {
  core::BarSource &Bars;
  core::CalendarSource &Calendar;
  core::RuoniaSource &Ruonia;
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
} // namespace worker
