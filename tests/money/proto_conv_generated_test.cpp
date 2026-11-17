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
  for (const char *Text :
       {"0", "0.000000001", "-0.000000001", "-0.5", "100.16",
        "9223372036854775807", "9223372036854775807.999999999",
        "-9223372036854775807.999999999"}) {
    const Decimal D = Decimal::parse(Text);
    const auto Msg = core::toProto<ProtoDecimal>(D);
    std::string Wire;
    ASSERT_TRUE(Msg.SerializeToString(&Wire));
    ProtoDecimal Back;
    ASSERT_TRUE(Back.ParseFromString(Wire));
    EXPECT_EQ(core::fromProto(Back), D) << Text;
    EXPECT_EQ(Back.units(), Msg.units());
    EXPECT_EQ(Back.nano(), Msg.nano());
  }
}

TEST(DecimalGeneratedProto, SignOfNanoFollowsUnits) {
  const auto Neg = core::toProto<ProtoDecimal>(Decimal::parse("-1.25"));
  EXPECT_EQ(Neg.units(), -1);
  EXPECT_EQ(Neg.nano(), -250000000);
  const auto Small = core::toProto<ProtoDecimal>(Decimal::parse("-0.25"));
  EXPECT_EQ(Small.units(), 0);
  EXPECT_EQ(Small.nano(), -250000000);

  ProtoDecimal Wrong;
  Wrong.set_units(1);
  Wrong.set_nano(-1);
  EXPECT_THROW(core::fromProto(Wrong), std::invalid_argument);
}
