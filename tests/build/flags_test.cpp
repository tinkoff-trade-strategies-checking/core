#include <gtest/gtest.h>

#include "dsl/check.hpp"
#include "dsl/interpreter.hpp"

// INV-4: сборка с -ffp-contract=off. Без него компилятор вправе слить a * b + c
// в одну операцию FMA и получить другой результат на другом процессоре.
TEST(BuildFlags, NoFusedMultiplyAdd) {
  // a * b = 1 - 2^-54 не представимо в double и округляется до 1, поэтому
  // раздельные умножение и сложение дают ровно 0; с FMA результат был бы
  // -2^-54.
  volatile double A = 1.0 + 0x1p-27;
  volatile double B = 1.0 - 0x1p-27;
  volatile double C = -1.0;
  EXPECT_EQ(A * B + C, 0.0);
}

// INV-4: подключённая библиотека dsl также собрана с -ffp-contract=off и
// корректно связывается с ядром.
TEST(BuildFlags, DslNoFusedMultiplyAddAndLinks) {
  EXPECT_EQ(dsl::formatBecauseNumber(1.005), "1,01");

  const char *const Source = "rule \"no_fma\" when close * open - 1 == 0 then "
                             "buy 1 lots because \"ok\"";
  const dsl::CheckResult Checked = dsl::check(Source);
  EXPECT_TRUE(Checked.Diagnostics.empty());

  const dsl::CompileResult Compiled = dsl::compile(Source, {}, 1);
  ASSERT_TRUE(Compiled.Ok);
  ASSERT_NE(Compiled.Instance, nullptr);

  const dsl::Bar StepBar{0, 1.0 - 0x1p-27, 2.0, 0.5, 1.0 + 0x1p-27, 100.0};
  const dsl::StepOutcome Outcome =
      Compiled.Instance->step(0, StepBar, dsl::PortfolioSnapshot{}, false);
  EXPECT_FALSE(Outcome.Error.has_value());
  ASSERT_EQ(Outcome.Intents.size(), 1U);
  EXPECT_EQ(Outcome.Intents[0].RuleName, "no_fma");
}
