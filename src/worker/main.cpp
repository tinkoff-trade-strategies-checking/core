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

std::atomic<bool> g_stop_requested{false};

extern "C" void handle_signal(int /*signal*/) { g_stop_requested.store(true); }

} // namespace

int main(int argc, char **argv) {
  std::string address = "0.0.0.0:50051";
  if (const char *env = std::getenv("WORKER_ADDRESS");
      env != nullptr && *env != '\0') {
    address = env;
  }
  if (argc == 3 && std::string(argv[1]) == "--listen") {
    address = argv[2];
  } else if (argc != 1) {
    std::cerr << "Использование: simulation_worker [--listen host:port]\n";
    return 2;
  }

  worker::ClickHouseConfig clickhouse;
  if (const char *env = std::getenv("CLICKHOUSE_URL");
      env != nullptr && *env != '\0') {
    try {
      clickhouse = worker::ClickHouseConfig::fromUrl(env);
    } catch (const std::invalid_argument &error) {
      std::cerr << "Неверный CLICKHOUSE_URL: " << error.what() << '\n';
      return 2;
    }
  } else {
    std::cerr
        << "ВНИМАНИЕ: CLICKHOUSE_URL не задан, бары и календарь читаются с "
        << clickhouse.baseUrl << " (база " << clickhouse.database
        << ", без учётных данных)" << '\n';
  }
  worker::ClickHouseBarSource bars(clickhouse);
  worker::ClickHouseCalendarSource calendar(clickhouse);
  worker::WorkerService service(worker::Context{bars, calendar});
  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) {
    std::cerr << "Не удалось запустить сервер на " << address << "\n";
    return 1;
  }
  std::cout << "simulation_worker слушает " << address << std::endl;

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  // В обработчике сигнала только флаг; остановку делает отдельный поток.
  std::thread stopper([&server] {
    while (!g_stop_requested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server->Shutdown();
  });
  server->Wait();
  g_stop_requested.store(true);
  stopper.join();
  return 0;
}
