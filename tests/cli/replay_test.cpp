#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>

namespace {

struct CliOutput {
  int ExitCode = -1;
  std::string Stdout;
};

CliOutput runCli(const std::string &Args) {
  const std::string Cmd = std::string(SIM_CLI_PATH) + " " + Args;
  std::FILE *Pipe = ::popen(Cmd.c_str(), "r");
  if (!Pipe)
    return CliOutput{-1, ""};
  std::string Out;
  char Buffer[4096];
  while (std::fgets(Buffer, sizeof(Buffer), Pipe) != nullptr)
    Out += Buffer;
  const int Status = ::pclose(Pipe);
  const int Code = WIFEXITED(Status) ? WEXITSTATUS(Status) : -1;
  return CliOutput{Code, Out};
}

TEST(CliReplay, FixtureProducesValidReportJson) {
  const CliOutput Res = runCli("replay --fixture");
  ASSERT_EQ(Res.ExitCode, 0) << Res.Stdout;
  EXPECT_NE(Res.Stdout.find("\"status\""), std::string::npos);
  EXPECT_NE(Res.Stdout.find("\"metrics\""), std::string::npos);
  EXPECT_NE(Res.Stdout.find("\"total_return\""), std::string::npos);
  EXPECT_NE(Res.Stdout.find("\"costs\""), std::string::npos);
  EXPECT_NE(Res.Stdout.find("\"fills\""), std::string::npos);
  EXPECT_NE(Res.Stdout.find("\"round_trips\""), std::string::npos);
  EXPECT_NE(Res.Stdout.find("\"final_equity\""), std::string::npos);
  EXPECT_NE(Res.Stdout.find("\"FIXA\""), std::string::npos);
}

TEST(CliReplay, JsonFileInputProducesReportJson) {
  const std::filesystem::path TempPath =
      std::filesystem::temp_directory_path() / "sim_cli_replay_test.json";
  {
    std::ofstream Out(TempPath);
    Out << R"json({
  "market": {
    "snapshot_id": "fixture-1",
    "timeframe": "D1",
    "from_date": "2024-01-09",
    "to_date": "2024-01-22",
    "initial_cash": "100000"
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
      "step": 3,
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

  const CliOutput Res = runCli("replay \"" + TempPath.string() + "\"");
  std::filesystem::remove(TempPath);
  ASSERT_EQ(Res.ExitCode, 0) << Res.Stdout;
  EXPECT_NE(Res.Stdout.find("\"metrics\""), std::string::npos);
  EXPECT_NE(Res.Stdout.find("\"fills\""), std::string::npos);
  EXPECT_NE(Res.Stdout.find("\"round_trips\""), std::string::npos);
  EXPECT_NE(Res.Stdout.find("\"entry_step\": 1"), std::string::npos);
  EXPECT_NE(Res.Stdout.find("\"exit_step\": 4"), std::string::npos);
}

TEST(CliReplay, MissingFileReturnsNonZero) {
  const CliOutput Res =
      runCli("replay /nonexistent/path/to/missing.json 2>/dev/null");
  EXPECT_NE(Res.ExitCode, 0);
}

TEST(CliReplay, InvalidJsonReturnsNonZero) {
  const std::filesystem::path TempPath =
      std::filesystem::temp_directory_path() / "sim_cli_bad_test.json";
  {
    std::ofstream Out(TempPath);
    Out << "{ not valid json";
  }
  const CliOutput Res =
      runCli("replay \"" + TempPath.string() + "\" 2>/dev/null");
  std::filesystem::remove(TempPath);
  EXPECT_NE(Res.ExitCode, 0);
}

} // namespace
