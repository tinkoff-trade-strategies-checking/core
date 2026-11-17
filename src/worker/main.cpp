// simulation_worker — gRPC-сервер Simulation Worker.
// Адрес: --listen host:port, иначе $WORKER_ADDRESS, иначе 0.0.0.0:50051.
// Бары и календарь: ClickHouse по $CLICKHOUSE_URL
// (http://user:password@host:8123/база).
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "clickhouse/clickhouse_bar_source.hpp"
#include "clickhouse/clickhouse_calendar_source.hpp"
#include "worker_service.hpp"

namespace {

std::atomic<bool> GStopRequested{false};

extern "C" void handleSignal(int /*signal*/) { GStopRequested.store(true); }

} // namespace

int main(int argc, char **argv) {
  std::string Address = "0.0.0.0:50051";
  if (const char *Env = std::getenv("WORKER_ADDRESS");
      Env != nullptr && *Env != '\0') {
    Address = Env;
  }
  if (argc == 3 && std::string(argv[1]) == "--listen") {
    Address = argv[2];
  } else if (argc != 1) {
    std::cerr << "Использование: simulation_worker [--listen host:port]\n";
    return 2;
  }

  worker::ClickHouseConfig Clickhouse;
  if (const char *Env = std::getenv("CLICKHOUSE_URL");
      Env != nullptr && *Env != '\0') {
    try {
      Clickhouse = worker::ClickHouseConfig::fromUrl(Env);
    } catch (const std::invalid_argument &Error) {
      std::cerr << "Неверный CLICKHOUSE_URL: " << Error.what() << '\n';
      return 2;
    }
  } else {
    std::cerr
        << "ВНИМАНИЕ: CLICKHOUSE_URL не задан, бары и календарь читаются с "
        << Clickhouse.BaseUrl << " (база " << Clickhouse.Database
        << ", без учётных данных)" << '\n';
  }
  worker::ClickHouseBarSource Bars(Clickhouse);
  worker::ClickHouseCalendarSource Calendar(Clickhouse);
  worker::WorkerService Service(worker::Context{Bars, Calendar});
  grpc::ServerBuilder Builder;
  Builder.AddListeningPort(Address, grpc::InsecureServerCredentials());
  Builder.RegisterService(&Service);
  std::unique_ptr<grpc::Server> Server = Builder.BuildAndStart();
  if (!Server) {
    std::cerr << "Не удалось запустить сервер на " << Address << "\n";
    return 1;
  }
  std::cout << "simulation_worker слушает " << Address << std::endl;

  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  // В обработчике сигнала только флаг; остановку делает отдельный поток.
  std::thread Stopper([&Server] {
    while (!GStopRequested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Server->Shutdown();
  });
  Server->Wait();
  GStopRequested.store(true);
  Stopper.join();
  return 0;
}
