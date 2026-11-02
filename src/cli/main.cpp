// sim_cli — CLI для replay и ручных прогонов Simulation Core. Пока заглушка:
// настоящий replay (данные, стратегия, журнал заявок) появится вместе с рыночным циклом.
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include "core/deterministic_run.hpp"
#include "core/version.hpp"

namespace {

void print_usage(std::ostream& out) {
  out << "Использование:\n"
         "  sim_cli version\n"
         "  sim_cli replay [--seed N] [--steps N]   заглушечный детерминированный прогон\n";
}

int run_replay(int argc, char** argv) {
  core::RunConfig config;
  config.steps = 10;
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if ((arg == "--seed" || arg == "--steps") && i + 1 < argc) {
      const std::uint64_t value = std::stoull(argv[++i]);
      if (arg == "--seed") {
        config.seed = value;
      } else {
        config.steps = static_cast<std::uint32_t>(value);
      }
    } else {
      std::cerr << "Неизвестный аргумент: " << arg << "\n";
      print_usage(std::cerr);
      return 2;
    }
  }
  const core::RunResult result = core::run_stub(config);
  std::cout << "seed=" << config.seed << " steps=" << config.steps
            << " checksum=" << result.checksum << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      print_usage(std::cerr);
      return 2;
    }
    const std::string_view command = argv[1];
    if (command == "version") {
      std::cout << core::version() << "\n";
      return 0;
    }
    if (command == "replay") {
      return run_replay(argc, argv);
    }
    print_usage(std::cerr);
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "Ошибка: " << e.what() << "\n";
    return 1;
  }
}
