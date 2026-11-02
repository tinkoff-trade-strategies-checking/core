#pragma once

#include <string>
#include <vector>

#include "core/market/calendar_source.hpp"
#include "core/market/in_memory_bar_source.hpp"
#include "core/market/instrument.hpp"

// Общие фикстуры тестов (входят и в core_tests, и в worker_tests).

namespace core::testing {

struct Fixture {
  InMemoryBarSource bars;
  InMemoryCalendarSource calendar;
  std::vector<Instrument> instruments;
  std::string snapshotId;
};

// Снапшот "fixture-1", шаг D1, 10 торговых дней 2024-01-09…12, 15…19, 22.
// Бумага A (key "A", ticker "FIXA", лот 10, шаг 0.01): close 100, 101, 103, 106, 104, 101, 99, 102, 105, 108.
// Бумага B (key "B", ticker "FIXB", лот 10000, шаг 0.000005): цена 0.0200, бара 2024-01-12 (шаг 3) нет.
Fixture fixture1();

}  // namespace core::testing
