#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <utility>

#include "core/money/decimal.hpp"
#include "core/money/proto_conv.hpp"

// Заглушка с теми же методами, что у сгенерированного
// pac.simulation.v1.Decimal: проверяем правила знаков ADR-0008 без protobuf.
// Настоящий тип - в worker_tests.
namespace {

struct FakeProtoDecimal {
  std::int64_t units_ = 0;
  std::int32_t nano_ = 0;
  std::int64_t units() const { return units_; }
  std::int32_t nano() const { return nano_; }
  void set_units(std::int64_t v) { units_ = v; }
  void set_nano(std::int32_t v) { nano_ = v; }
};

using core::Decimal;
using core::UnitsNano;

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();

} // namespace

TEST(DecimalUnitsNano, SignRulesAndExtremes) {
  struct Case {
    const char *text;
    std::int64_t units;
    std::int32_t nano;
  };
  const Case cases[] = {
      {"0", 0, 0},
      {"0.000000001", 0, 1},
      {"-0.000000001", 0, -1},
      {"-0.5", 0, -500000000},
      {"1.5", 1, 500000000},
      {"-1.5", -1, -500000000},
      {"-3", -3, 0},
      {"0.999999999", 0, 999999999},
      {"9223372036854775807.999999999", kInt64Max, 999999999},
      {"-9223372036854775807.999999999", -kInt64Max, -999999999},
      {"9223372036854775807", kInt64Max, 0},
      {"-9223372036854775808", std::numeric_limits<std::int64_t>::min(), 0},
  };
  for (const auto &c : cases) {
    const Decimal d = Decimal::parse(c.text);
    EXPECT_EQ(d.toUnitsNano(), (UnitsNano{c.units, c.nano})) << c.text;
    EXPECT_EQ(Decimal::fromUnitsNano(c.units, c.nano), d) << c.text;
  }
}

TEST(DecimalUnitsNano, RejectsInvalidPairs) {
  EXPECT_THROW(Decimal::fromUnitsNano(1, -1),
               std::invalid_argument); // знаки разные
  EXPECT_THROW(Decimal::fromUnitsNano(-1, 1), std::invalid_argument);
  EXPECT_THROW(Decimal::fromUnitsNano(0, 1000000000),
               std::invalid_argument); // |nano| > 999 999 999
  EXPECT_THROW(Decimal::fromUnitsNano(0, -1000000000), std::invalid_argument);
  EXPECT_THROW(
      Decimal::fromUnitsNano(0, std::numeric_limits<std::int32_t>::min()),
      std::invalid_argument);
  EXPECT_NO_THROW(Decimal::fromUnitsNano(0, -999999999));
}

TEST(DecimalUnitsNano, ValueOutsideInt64UnitsOverflows) {
  EXPECT_THROW(Decimal::parse("9223372036854775808").toUnitsNano(),
               core::DecimalOverflow);
  EXPECT_THROW(Decimal::parse("-9223372036854775809").toUnitsNano(),
               core::DecimalOverflow);
}

TEST(DecimalProtoTemplate, RoundTripThroughAccessors) {
  for (const char *text :
       {"0", "123.45", "-0.000000001", "9223372036854775807.999999999",
        "-9223372036854775807.999999999"}) {
    const Decimal d = Decimal::parse(text);
    const auto msg = core::toProto<FakeProtoDecimal>(d);
    EXPECT_EQ(core::fromProto(msg), d) << text;
  }
  FakeProtoDecimal bad;
  bad.set_units(5);
  bad.set_nano(-5);
  EXPECT_THROW(core::fromProto(bad), std::invalid_argument);
}
