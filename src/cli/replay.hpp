#pragma once

#include <iosfwd>

namespace cli {

void printReplayUsage(std::ostream &Out);

// Запускает команду `sim_cli replay ...`. Возвращает код возврата процесса.
int runReplay(int Argc, char **Argv, std::ostream &Out, std::ostream &Err);

} // namespace cli
