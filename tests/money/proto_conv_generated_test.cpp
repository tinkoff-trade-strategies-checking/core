#include <gtest/gtest.h>

#include <string>

#include "common.pb.h"
#include "core/money/proto_conv.hpp"

// Конвертация с настоящим сообщением pac.simulation.v1.Decimal
// (proto/common.proto), включая проход через бинарную сериализацию protobuf
// (ADR-0008, раздел «Проверка»).

using core::Decimal;
using ProtoDecimal = pac::simulation::v1::Decimal;

TEST(DecimalGeneratedProto, ExtremesRoundTripThroughWire) {
  for (const char *text :
       {"0", "0.000000001", "-0.000000001", "-0.5", "100.16",
        "9223372036854775807", "9223372036854775807.999999999",
        "-9223372036854775807.999999999"}) {
    const Decimal d = Decimal::parse(text);
    const auto msg = core::toProto<ProtoDecimal>(d);
    std::string wire;
    ASSERT_TRUE(msg.SerializeToString(&wire));
    ProtoDecimal back;
    ASSERT_TRUE(back.ParseFromString(wire));
    EXPECT_EQ(core::fromProto(back), d) << text;
    EXPECT_EQ(back.units(), msg.units());
    EXPECT_EQ(back.nano(), msg.nano());
  }
}

TEST(DecimalGeneratedProto, SignOfNanoFollowsUnits) {
  const auto neg = core::toProto<ProtoDecimal>(Decimal::parse("-1.25"));
  EXPECT_EQ(neg.units(), -1);
  EXPECT_EQ(neg.nano(), -250000000);
  const auto small = core::toProto<ProtoDecimal>(Decimal::parse("-0.25"));
  EXPECT_EQ(small.units(), 0);
  EXPECT_EQ(small.nano(), -250000000);

  ProtoDecimal wrong;
  wrong.set_units(1);
  wrong.set_nano(-1);
  EXPECT_THROW(core::fromProto(wrong), std::invalid_argument);
}
