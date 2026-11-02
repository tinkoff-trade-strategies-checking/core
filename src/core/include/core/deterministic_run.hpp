#pragma once

#include <cstdint>
#include <vector>

namespace core {

// Параметры заглушечного прогона. Настоящие данные, стратегия и параметры придут позже.
struct RunConfig {
  std::uint64_t seed = 0;
  std::uint32_t steps = 0;
};

// Результат прогона: ряд значений по шагам и контрольная сумма.
// Целые числа вместо double: результат сравнивается побитово (INV-4).
struct RunResult {
  std::vector<std::int64_t> series;
  std::uint64_t checksum = 0;
};

// Тривиальный детерминированный расчёт: случайность только из явного seed (SplitMix64),
// без системных часов и hash-контейнеров. Каркас под тест детерминизма (INV-4);
// будет заменён настоящим рыночным циклом.
RunResult run_stub(const RunConfig& config);

// Канонический байтовый вид результата (порядок байт фиксирован: little-endian),
// чтобы сравнивать прогоны побитово независимо от платформы.
std::vector<std::uint8_t> serialize(const RunResult& result);

}  // namespace core
