// sim_cli — CLI для replay и ручных прогонов Simulation Core. Пока заглушка:
// прогоны выполняет воркер (`runBacktest`, `replaySession`), CLI для них появится позже.
#include <exception>
#include <iostream>
#include <string_view>

#include "core/version.hpp"

namespace {

void print_usage(std::ostream& out) {
  out << "Использование:\n"
         "  sim_cli version\n"
         "  sim_cli replay   пока не реализован (прогоны идут через воркер)\n";
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
      std::cerr << "replay пока не реализован\n";
      return 1;
    }
    print_usage(std::cerr);
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "Ошибка: " << e.what() << "\n";
    return 1;
  }
}
