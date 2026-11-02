// simulation_worker — gRPC-сервер Simulation Worker. Пока отвечает только на Ping.
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "worker_service.hpp"

namespace {

std::atomic<bool> g_stop_requested{false};

extern "C" void handle_signal(int /*signal*/) {
  g_stop_requested.store(true);
}

}  // namespace

int main(int argc, char** argv) {
  std::string address = "0.0.0.0:50051";
  if (argc == 3 && std::string(argv[1]) == "--listen") {
    address = argv[2];
  } else if (argc != 1) {
    std::cerr << "Использование: simulation_worker [--listen host:port]\n";
    return 2;
  }

  worker::WorkerService service;
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
