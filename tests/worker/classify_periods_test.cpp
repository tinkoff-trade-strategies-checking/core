#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "core/market/bar.hpp"
#include "core/market/bar_source.hpp"
#include "core/market/calendar_source.hpp"
#include "core/market/in_memory_bar_source.hpp"
#include "core/market/ruonia_source.hpp"
#include "core/market/timeframe.hpp"
#include "core/money/decimal.hpp"
#include "worker_service.hpp"
#include "worker_test_support.hpp"

namespace {

using namespace worker_test;

core::Bar dailyBar(std::int64_t TsMs, const char *Close) {
  core::Bar B;
  B.TsOpenMs = TsMs;
  B.AvailableAtMs = TsMs + 86400000LL;
  B.Open = core::Decimal::parse(Close);
  B.High = core::Decimal::parse(Close);
  B.Low = core::Decimal::parse(Close);
  B.Close = core::Decimal::parse(Close);
  B.Volume = 1000;
  return B;
}

} // namespace

TEST(WorkerClassifyPeriods, AllWindowsAndSingleWindowViaRpc) {
  core::InMemoryBarSource Bars;
  core::InMemoryCalendarSource Calendar;
  core::InMemoryRuoniaSource Ruonia;

  // 2025-01-06..2025-01-13 (6 торговых дней, L = 4 -> 3 окна).
  const std::int64_t Day0 = 1736110800000LL; // 2025-01-06 00:00 MSK
  const std::int64_t DayMs = 86400000LL;
  Bars.addSecurity("snap-1",
                   {"IMOEX", "IMOEX", 1, core::Decimal::parse("0.01")});
  Bars.addSeries("snap-1", "IMOEX", core::intervalSeconds(core::Timeframe::D1),
                 {
                     dailyBar(Day0 + 0 * DayMs, "100"),
                     dailyBar(Day0 + 1 * DayMs, "100"),
                     dailyBar(Day0 + 2 * DayMs, "100"),
                     dailyBar(Day0 + 3 * DayMs, "100"),
                     dailyBar(Day0 + 4 * DayMs, "105"),
                     dailyBar(Day0 + 5 * DayMs, "75"),
                 });
  std::vector<core::TradingDay> Days;
  for (int I = 0; I < 6; ++I)
    Days.push_back({20250106 + I, true});
  Calendar.add("snap-1", Days);

  worker::Context Ctx{Bars, Calendar, Ruonia};
  worker::WorkerService Service(Ctx);
  grpc::ServerBuilder Builder;
  Builder.RegisterService(&Service);
  std::unique_ptr<grpc::Server> Server = Builder.BuildAndStart();
  std::unique_ptr<pb::SimulationWorker::Stub> Stub =
      pb::SimulationWorker::NewStub(
          Server->InProcessChannel(grpc::ChannelArguments()));

  pb::ClassifyPeriodsRequest Req;
  Req.set_snapshot_id("snap-1");
  Req.set_window_length(4);
  Req.set_from_date("2025-01-06");
  Req.set_to_date("2025-01-11");

  pb::ClassifyPeriodsResponse AllResp;
  grpc::ClientContext ClientCtx1;
  ASSERT_TRUE(Stub->ClassifyPeriods(&ClientCtx1, Req, &AllResp).ok());
  ASSERT_EQ(AllResp.windows_size(), 3);
  EXPECT_EQ(AllResp.windows(0).start_date(), "2025-01-06");
  EXPECT_EQ(AllResp.windows(0).category(), pb::CALM);
  EXPECT_EQ(AllResp.windows(2).start_date(), "2025-01-08");
  EXPECT_EQ(AllResp.windows(2).category(), pb::STRESS);
  EXPECT_LE(AllResp.p33(), AllResp.p80());

  // Вариант «своя дата».
  Req.set_start_date("2025-01-08");
  pb::ClassifyPeriodsResponse SingleResp;
  grpc::ClientContext ClientCtx2;
  ASSERT_TRUE(Stub->ClassifyPeriods(&ClientCtx2, Req, &SingleResp).ok());
  ASSERT_EQ(SingleResp.windows_size(), 1);
  EXPECT_EQ(SingleResp.windows(0).SerializeAsString(),
            AllResp.windows(2).SerializeAsString());
  EXPECT_DOUBLE_EQ(SingleResp.p33(), AllResp.p33());
  EXPECT_DOUBLE_EQ(SingleResp.p80(), AllResp.p80());

  // Вариант «своя дата» с узким диапазоном from/to: пороги и категория
  // должны совпадать с полным снапшотом, а не вырождаться по одному окну.
  pb::ClassifyPeriodsRequest NarrowReq;
  NarrowReq.set_snapshot_id("snap-1");
  NarrowReq.set_window_length(4);
  NarrowReq.set_from_date("2025-01-06");
  NarrowReq.set_to_date("2025-01-09");

  pb::ClassifyPeriodsResponse NarrowResp;
  grpc::ClientContext ClientCtx3;
  ASSERT_TRUE(Stub->ClassifyPeriods(&ClientCtx3, NarrowReq, &NarrowResp).ok());
  ASSERT_EQ(NarrowResp.windows_size(), 1);
  EXPECT_DOUBLE_EQ(NarrowResp.p33(), AllResp.p33());
  EXPECT_DOUBLE_EQ(NarrowResp.p80(), AllResp.p80());
  EXPECT_EQ(NarrowResp.windows(0).SerializeAsString(),
            AllResp.windows(0).SerializeAsString());
  EXPECT_EQ(NarrowResp.windows(0).category(), pb::CALM);

  NarrowReq.set_start_date("2025-01-06");
  pb::ClassifyPeriodsResponse NarrowSingleResp;
  grpc::ClientContext ClientCtx4;
  ASSERT_TRUE(
      Stub->ClassifyPeriods(&ClientCtx4, NarrowReq, &NarrowSingleResp).ok());
  ASSERT_EQ(NarrowSingleResp.windows_size(), 1);
  EXPECT_DOUBLE_EQ(NarrowSingleResp.p33(), AllResp.p33());
  EXPECT_DOUBLE_EQ(NarrowSingleResp.p80(), AllResp.p80());
  EXPECT_EQ(NarrowSingleResp.windows(0).SerializeAsString(),
            AllResp.windows(0).SerializeAsString());

  Server->Shutdown();
}

TEST(WorkerClassifyPeriods, ErrorsOnInvalidInputAndMissingIndex) {
  core::testing::Fixture Fixture = core::testing::fixture1();
  worker::Context Ctx{Fixture.Bars, Fixture.Calendar, Fixture.Ruonia};

  pb::ClassifyPeriodsRequest Req;
  Req.set_snapshot_id("fixture-1");
  Req.set_window_length(2);
  Req.set_from_date("2024-01-09");
  Req.set_to_date("2024-01-22");
  pb::ClassifyPeriodsResponse Resp;
  EXPECT_EQ(worker::handleClassifyPeriods(Ctx, Req, &Resp).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  Req.set_window_length(3);
  // В fixture-1 нет бумаги IMOEX -> FAILED_PRECONDITION.
  EXPECT_EQ(worker::handleClassifyPeriods(Ctx, Req, &Resp).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
}
