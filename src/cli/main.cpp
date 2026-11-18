// sim_cli — CLI для replay и ручных прогонов Simulation Core.
#include <exception>
#include <iostream>
#include <string_view>

#include "core/version.hpp"
#include "replay.hpp"

namespace {

void printUsage(std::ostream &Out) {
  Out << "Использование:\n"
         "  sim_cli version              версия Simulation Core\n"
         "  sim_cli replay --fixture     прогон встроенной фикстуры fixture-1\n"
         "  sim_cli replay <файл.json>   прогон сценария из JSON-файла\n";
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
      return cli::runReplay(argc - 2, argv + 2, std::cout, std::cerr);
    }
    printUsage(std::cerr);
    return 2;
  } catch (const std::exception &E) {
    std::cerr << "Ошибка: " << E.what() << "\n";
    return 1;
  }
}
