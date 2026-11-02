#pragma once

#include <cstdint>
#include <memory>

#include <grpcpp/grpcpp.h>

#include "core/run/backtest.hpp"  // Progress

// Прогресс прогона в потоке ответа (RunBacktestUpdate, WalkForwardUpdate: у обоих поле progress).

namespace worker {

// Клиент закрыл поток: прогресс не уходит, считать дальше незачем. Обработчик ловит и отвечает CANCELLED.
struct StreamClosed {};

// Одно сообщение прогресса без прореживания. Закрытый поток - StreamClosed.
template <class Update>
void writeProgress(grpc::ServerWriterInterface<Update>* writer, std::int64_t done, std::int64_t total) {
  Update update;
  update.mutable_progress()->set_done_steps(done);
  update.mutable_progress()->set_total_steps(total);
  if (!writer->Write(update)) throw StreamClosed{};
}

// Колбэк для ядра: не чаще одного сообщения на 0,1 % работы, шагов в прогоне - до сотен тысяч.
// totalFactor > 1 растягивает шкалу: ядро считает свою часть работы, остальное (цепочка издержек) дописывает вызывающий.
template <class Update>
core::Progress progressWriter(grpc::ServerWriterInterface<Update>* writer, std::int64_t totalFactor = 1) {
  constexpr std::int64_t kProgressSlices = 1000;
  auto lastSlice = std::make_shared<std::int64_t>(-1);
  return [writer, lastSlice, totalFactor](std::int64_t done, std::int64_t total) {
    const std::int64_t slice = total > 0 ? done * kProgressSlices / total : kProgressSlices;
    if (slice == *lastSlice) return;
    *lastSlice = slice;
    writeProgress(writer, done, total * totalFactor);
  };
}

}  // namespace worker
