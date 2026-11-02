// Каркас теста детерминизма (INV-4): один и тот же вход даёт побитово тот же результат.
// Когда появится рыночный цикл, сюда добавляется прогон на эталонных данных.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "core/deterministic_run.hpp"

namespace {

core::RunConfig make_config(std::uint64_t seed) {
  core::RunConfig config;
  config.seed = seed;
  config.steps = 10000;
  return config;
}

}  // namespace

TEST(Determinism, RepeatedRunIsByteIdentical) {
  const core::RunConfig config = make_config(42);

  const std::vector<std::uint8_t> first = core::serialize(core::run_stub(config));
  const std::vector<std::uint8_t> second = core::serialize(core::run_stub(config));

  ASSERT_FALSE(first.empty());
  EXPECT_EQ(first, second);
}

TEST(Determinism, DifferentSeedGivesDifferentResult) {
  // Защита от вырожденного теста: результат вообще зависит от входа.
  const auto a = core::serialize(core::run_stub(make_config(1)));
  const auto b = core::serialize(core::run_stub(make_config(2)));
  EXPECT_NE(a, b);
}

TEST(Determinism, GoldenChecksumIsStable) {
  // Фиксирует значение между сборками и платформами; при намеренном изменении
  // алгоритма прогона обновляется вместе с ним.
  const core::RunResult result = core::run_stub(make_config(42));
  EXPECT_EQ(result.series.size(), 10000U);
  EXPECT_EQ(result.checksum, 7907853569645618218ULL);
}
