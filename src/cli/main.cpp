// sim_cli — CLI для replay и ручных прогонов Simulation Core. Пока заглушка:
// прогоны выполняет воркер (`runBacktest`, `replaySession`), CLI для них
// появится позже.
#include <exception>
#include <iostream>
#include <string_view>

#include "core/version.hpp"

namespace {

void printUsage(std::ostream &Out) {
  Out << "Использование:\n"
         "  sim_cli version\n"
         "  sim_cli replay   пока не реализован (прогоны идут через воркер)\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2) {
      printUsage(std::cerr);
      return 2;
    }
    const std::string_view Command = argv[1];
    if (Command == "version") {
      std::cout << core::version() << "\n";
      return 0;
    }
    if (Command == "replay") {
      std::cerr << "replay пока не реализован\n";
      return 1;
    }
    printUsage(std::cerr);
    return 2;
  } catch (const std::exception &E) {
    std::cerr << "Ошибка: " << E.what() << "\n";
    return 1;
  }
}
