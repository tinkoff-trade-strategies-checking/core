#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

#include "core/money/proto_conv.hpp"
#include "handlers.hpp"
#include "worker_test_support.hpp"

// Сверка sim_cli replay и обработчика ReplaySession воркера (core#50, I-2):
// один JSON-сценарий на fixture-1 с from_date = 2024-01-15 (предыдущий торговый
// день 2024-01-12, Delta_0 = 3) должен давать одинаковые метрики.

namespace {

using namespace worker_test;

struct CliOutput {
  int ExitCode = -1;
  std::string Stdout;
  std::string Stderr;
};

CliOutput runCli(const std::string &Args) {
  const auto *Info = ::testing::UnitTest::GetInstance()->current_test_info();
  const std::filesystem::path ErrPath =
      std::filesystem::temp_directory_path() /
      (std::string("sim_cli_parity_stderr_") + Info->test_suite_name() + "_" +
       Info->name() + "_" + std::to_string(::getpid()) + ".txt");
  const std::string Cmd = std::string(SIM_CLI_PATH) + " " + Args + " 2>\"" +
                          ErrPath.string() + "\"";
  std::FILE *Pipe = ::popen(Cmd.c_str(), "r");
  if (!Pipe)
    return CliOutput{-1, "", ""};
  std::string Out;
  char Buffer[4096];
  while (std::fgets(Buffer, sizeof(Buffer), Pipe) != nullptr)
    Out += Buffer;
  const int Status = ::pclose(Pipe);
  const int Code = WIFEXITED(Status) ? WEXITSTATUS(Status) : -1;
  std::string Err;
  {
    std::ifstream ErrFile(ErrPath);
    if (ErrFile) {
      Err.assign(std::istreambuf_iterator<char>(ErrFile),
                 std::istreambuf_iterator<char>());
    }
  }
  std::filesystem::remove(ErrPath);
  return CliOutput{Code, Out, Err};
}

// Извлекает значение поля из плоского фрагмента JSON ("key": value).
std::string extractJsonRaw(std::string_view Json, std::string_view Section,
                           std::string_view Key) {
  std::size_t Start = 0;
  if (!Section.empty()) {
    const std::string SecNeedle = "\"" + std::string(Section) + "\":";
    const std::size_t SecPos = Json.find(SecNeedle);
    if (SecPos == std::string_view::npos)
      throw std::runtime_error("раздел не найден: " + std::string(Section));
    Start = SecPos + SecNeedle.size();
  }
  const std::string Needle = "\"" + std::string(Key) + "\":";
  const std::size_t Pos = Json.find(Needle, Start);
  if (Pos == std::string_view::npos)
    throw std::runtime_error("ключ не найден: " + std::string(Key));
  std::size_t ValStart = Pos + Needle.size();
  while (ValStart < Json.size() &&
         (Json[ValStart] == ' ' || Json[ValStart] == '\t' ||
          Json[ValStart] == '\n' || Json[ValStart] == '\r')) {
    ++ValStart;
  }
  std::size_t ValEnd = ValStart;
  if (ValStart < Json.size() && Json[ValStart] == '"') {
    ++ValEnd;
    while (ValEnd < Json.size() && Json[ValEnd] != '"')
      ++ValEnd;
    return std::string(Json.substr(ValStart + 1, ValEnd - ValStart - 1));
  }
  while (ValEnd < Json.size() && Json[ValEnd] != ',' && Json[ValEnd] != '}' &&
         Json[ValEnd] != '\n' && Json[ValEnd] != '\r') {
    ++ValEnd;
  }
  return std::string(Json.substr(ValStart, ValEnd - ValStart));
}

std::optional<double> jsonOptDouble(std::string_view Json,
                                    std::string_view Section,
                                    std::string_view Key) {
  const std::string Raw = extractJsonRaw(Json, Section, Key);
  if (Raw == "null")
    return std::nullopt;
  return std::stod(Raw);
}

std::int64_t jsonInt(std::string_view Json, std::string_view Section,
                     std::string_view Key) {
  return std::stoll(extractJsonRaw(Json, Section, Key));
}

void verifyParity(const std::string &BenchmarkTicker) {
  const auto *Info = ::testing::UnitTest::GetInstance()->current_test_info();
  const std::filesystem::path TempPath =
      std::filesystem::temp_directory_path() /
      (std::string("sim_cli_parity_") + Info->name() + "_" +
       std::to_string(::getpid()) + ".json");
  {
    std::ofstream Out(TempPath);
    Out << R"json({
  "market": {
    "snapshot_id": "fixture-1",
    "timeframe": "D1",
    "from_date": "2024-01-15",
    "to_date": "2024-01-22",
    "initial_cash": "100000",
    "costs": {
      "commission_rate": "0.0005",
      "exchange_fee_rate": "0.0001",
      "spread": "0.001",
      "slippage_kappa": "0.1",
      "liquidity_share": "0.10"
    })json";
    if (!BenchmarkTicker.empty())
      Out << ",\n    \"benchmark_ticker\": \"" << BenchmarkTicker << "\"";
    Out << R"json(
  },
  "journal": [
    {
      "seq": 0,
      "step": 0,
      "place": {
        "order": {
          "instrument_key": "A",
          "side": "BUY",
          "type": "MARKET",
          "lots": 5,
          "time_in_force": "GTC"
        }
      }
    },
    {
      "seq": 1,
      "step": 2,
      "place": {
        "order": {
          "instrument_key": "A",
          "side": "SELL",
          "type": "MARKET",
          "lots": 5,
          "time_in_force": "GTC"
        }
      }
    }
  ]
})json";
  }

  const CliOutput CliRes = runCli("replay \"" + TempPath.string() + "\"");
  std::filesystem::remove(TempPath);
  ASSERT_EQ(CliRes.ExitCode, 0) << CliRes.Stderr << "\n" << CliRes.Stdout;

  core::testing::Fixture Fix = core::testing::fixture1();
  worker::Context Ctx{Fix.Bars, Fix.Calendar, Fix.Ruonia};

  pb::ReplaySessionRequest Req;
  *Req.mutable_market() = fixtureMarket();
  Req.mutable_market()->set_from_date("2024-01-15");
  Req.mutable_market()->set_to_date("2024-01-22");
  *Req.mutable_market()->mutable_initial_cash() = dec("100000");
  if (!BenchmarkTicker.empty())
    Req.mutable_market()->set_benchmark_ticker(BenchmarkTicker);
  *Req.add_journal() = marketOrder(0, 0, "A", pb::BUY, 5);
  *Req.add_journal() = marketOrder(1, 2, "A", pb::SELL, 5);
  // 2024-01-15..19, 22 — 6 торговых дней (шаги 0..5).
  Req.set_up_to_step(5);
  Req.set_events_from_step(-1);
  Req.set_include_report(true);

  pb::ReplaySessionResponse Resp;
  const grpc::Status Status = worker::handleReplaySession(Ctx, Req, &Resp);
  ASSERT_TRUE(Status.ok()) << Status.error_message();
  ASSERT_TRUE(Resp.has_report());

  const pb::Metrics &M = Resp.report().metrics();
  constexpr double kEps = 1e-11;

  auto ExpectOptEq = [&](std::string_view Key, bool Has, double WorkerVal) {
    const auto CliVal = jsonOptDouble(CliRes.Stdout, "metrics", Key);
    ASSERT_EQ(CliVal.has_value(), Has) << "поле " << Key;
    if (Has) {
      EXPECT_NEAR(*CliVal, WorkerVal, kEps) << "поле " << Key;
    }
  };

  EXPECT_NEAR(*jsonOptDouble(CliRes.Stdout, "metrics", "total_return"),
              M.total_return(), kEps);
  EXPECT_NEAR(*jsonOptDouble(CliRes.Stdout, "metrics", "gross_return"),
              M.gross_return(), kEps);
  ExpectOptEq("cagr", M.has_cagr(), M.cagr());
  EXPECT_NEAR(*jsonOptDouble(CliRes.Stdout, "metrics", "max_drawdown"),
              M.max_drawdown(), kEps);
  EXPECT_EQ(jsonInt(CliRes.Stdout, "metrics", "max_drawdown_steps"),
            M.max_drawdown_steps());
  ExpectOptEq("volatility", M.has_volatility(), M.volatility());
  ASSERT_TRUE(M.has_sharpe());
  ASSERT_TRUE(M.has_sortino());
  ExpectOptEq("sharpe", M.has_sharpe(), M.sharpe());
  ExpectOptEq("sortino", M.has_sortino(), M.sortino());
  EXPECT_NEAR(*jsonOptDouble(CliRes.Stdout, "metrics", "turnover"),
              M.turnover(), kEps);
  EXPECT_EQ(jsonInt(CliRes.Stdout, "metrics", "fills"), M.fills());
  EXPECT_EQ(jsonInt(CliRes.Stdout, "metrics", "round_trips"), M.round_trips());
  ExpectOptEq("win_rate", M.has_win_rate(), M.win_rate());
  EXPECT_EQ(jsonInt(CliRes.Stdout, "metrics", "rejections"), M.rejections());
  EXPECT_EQ(jsonInt(CliRes.Stdout, "metrics", "partial_fills"),
            M.partial_fills());
  EXPECT_NEAR(*jsonOptDouble(CliRes.Stdout, "metrics", "hhi_avg"), M.hhi_avg(),
              kEps);
  EXPECT_NEAR(*jsonOptDouble(CliRes.Stdout, "metrics", "max_position_share"),
              M.max_position_share(), kEps);
  ExpectOptEq("benchmark_return", M.has_benchmark_return(),
              M.benchmark_return());
  EXPECT_EQ(jsonInt(CliRes.Stdout, "metrics", "trading_days"),
            M.trading_days());
  EXPECT_EQ(jsonInt(CliRes.Stdout, "metrics", "stop_take_same_bar"),
            M.stop_take_same_bar());

  EXPECT_EQ(extractJsonRaw(CliRes.Stdout, "", "final_equity"),
            core::fromProto(Resp.report().final_equity()).toString());
  EXPECT_EQ(extractJsonRaw(CliRes.Stdout, "costs", "commission"),
            core::fromProto(Resp.report().costs().commission()).toString());
  EXPECT_EQ(extractJsonRaw(CliRes.Stdout, "costs", "fee"),
            core::fromProto(Resp.report().costs().exchange_fee()).toString());
  EXPECT_EQ(extractJsonRaw(CliRes.Stdout, "costs", "spread"),
            core::fromProto(Resp.report().costs().spread()).toString());
  EXPECT_EQ(extractJsonRaw(CliRes.Stdout, "costs", "slippage"),
            core::fromProto(Resp.report().costs().slippage()).toString());
}

TEST(CliWorkerParity, SameScenarioFrom20240115MatchesWorkerMetrics) {
  verifyParity("");
}

TEST(CliWorkerParity, SameScenarioWithBenchmarkMatchesWorkerMetrics) {
  verifyParity("FIXA");
}

} // namespace
