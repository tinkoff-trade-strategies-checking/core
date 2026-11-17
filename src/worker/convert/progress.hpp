#pragma once

#include <cstdint>
#include <memory>

#include <grpcpp/grpcpp.h>

#include "core/run/backtest.hpp" // Progress

// Прогресс прогона в потоке ответа (RunBacktestUpdate, WalkForwardUpdate: у
// обоих поле progress).

namespace worker {

// Клиент закрыл поток: прогресс не уходит, считать дальше незачем. Обработчик
// ловит и отвечает CANCELLED.
struct StreamClosed {};

// Одно сообщение прогресса без прореживания. Закрытый поток - StreamClosed.
template <class UpdateT>
void writeProgress(grpc::ServerWriterInterface<UpdateT> *Writer,
                   std::int64_t Done, std::int64_t Total) {
  UpdateT Update;
  Update.mutable_progress()->set_done_steps(Done);
  Update.mutable_progress()->set_total_steps(Total);
  if (!Writer->Write(Update))
    throw StreamClosed{};
}

// Колбэк для ядра: не чаще одного сообщения на 0,1 % работы, шагов в прогоне -
// до сотен тысяч. totalFactor > 1 растягивает шкалу: ядро считает свою часть
// работы, остальное (цепочка издержек) дописывает вызывающий.
template <class Update>
core::Progress progressWriter(grpc::ServerWriterInterface<Update> *Writer,
                              std::int64_t TotalFactor = 1) {
  constexpr std::int64_t kProgressSlices = 1000;
  auto LastSlice = std::make_shared<std::int64_t>(-1);
  return
      [Writer, LastSlice, TotalFactor](std::int64_t Done, std::int64_t Total) {
        const std::int64_t Slice =
            Total > 0 ? Done * kProgressSlices / Total : kProgressSlices;
        if (Slice == *LastSlice)
          return;
        *LastSlice = Slice;
        writeProgress(Writer, Done, Total * TotalFactor);
      };
}

} // namespace worker
