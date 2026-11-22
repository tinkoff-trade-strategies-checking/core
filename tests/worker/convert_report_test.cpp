#include <gtest/gtest.h>

#include <stdexcept>

#include "convert/report.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/report/metrics.hpp"
#include "support/fixture1.hpp"

// Перевод отчёта ядра (CheckLevel, Finding, Report) в proto (ФТ-14, ФТ-17).

namespace {

namespace pb = pac::simulation::v1;

core::Timeline reportTimeline() {
  auto F = core::testing::fixture1();
  return core::Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                              core::Timeframe::D1, 20240109, 20240122);
}

} // namespace

TEST(ConvertReport, EveryCheckLevelMapsBothWays) {
  EXPECT_EQ(worker::toProto(core::CheckLevel::Ok), pb::OK);
  EXPECT_EQ(worker::toProto(core::CheckLevel::Warning), pb::WARNING);
  EXPECT_EQ(worker::toProto(core::CheckLevel::Error), pb::ERROR);

  EXPECT_EQ(worker::checkLevelFromProto(pb::OK), core::CheckLevel::Ok);
  EXPECT_EQ(worker::checkLevelFromProto(pb::WARNING),
            core::CheckLevel::Warning);
  EXPECT_EQ(worker::checkLevelFromProto(pb::ERROR), core::CheckLevel::Error);

  EXPECT_THROW(worker::checkLevelFromProto(pb::LEVEL_UNSPECIFIED),
               std::invalid_argument);
}

TEST(ConvertReport, ReportMapsLevelAndFindings) {
  const core::Timeline Tl = reportTimeline();
  core::Report Report;
  Report.FinalEquity = core::Decimal::fromUnits(100000);
  Report.ReportLevel = core::CheckLevel::Error;
  Report.Findings = {
      core::Finding{"W-RT01", core::CheckLevel::Warning, "угаданные гэпы", 5},
      core::Finding{"E-RT01", core::CheckLevel::Error, "нарушение порядка",
                    -1},
  };

  pb::Report Proto;
  worker::toProto(Report, Tl, &Proto);
  EXPECT_EQ(Proto.level(), pb::ERROR);
  ASSERT_EQ(Proto.findings_size(), 2);

  EXPECT_EQ(Proto.findings(0).code(), "W-RT01");
  EXPECT_EQ(Proto.findings(0).level(), pb::WARNING);
  EXPECT_EQ(Proto.findings(0).message(), "угаданные гэпы");
  EXPECT_EQ(Proto.findings(0).step(), 5);

  EXPECT_EQ(Proto.findings(1).code(), "E-RT01");
  EXPECT_EQ(Proto.findings(1).level(), pb::ERROR);
  EXPECT_EQ(Proto.findings(1).message(), "нарушение порядка");
  EXPECT_EQ(Proto.findings(1).step(), -1);
}
