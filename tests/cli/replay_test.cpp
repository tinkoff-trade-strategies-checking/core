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
  std::string Stderr;
};

CliOutput runCli(const std::string &Args) {
  const std::filesystem::path ErrPath =
      std::filesystem::temp_directory_path() / "sim_cli_test_stderr.txt";
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
  const CliOutput Res = runCli("replay /nonexistent/path/to/missing.json");
  EXPECT_NE(Res.ExitCode, 0);
  EXPECT_FALSE(Res.Stderr.empty());
}

TEST(CliReplay, InvalidJsonReturnsNonZero) {
  const std::filesystem::path TempPath =
      std::filesystem::temp_directory_path() / "sim_cli_bad_test.json";
  {
    std::ofstream Out(TempPath);
    Out << "{ not valid json";
  }
  const CliOutput Res = runCli("replay \"" + TempPath.string() + "\"");
  std::filesystem::remove(TempPath);
  EXPECT_NE(Res.ExitCode, 0);
  EXPECT_FALSE(Res.Stderr.empty());
}

TEST(CliReplay, RoundTripsIncludeCosts) {
  const CliOutput Res = runCli("replay --fixture");
  ASSERT_EQ(Res.ExitCode, 0) << Res.Stderr;
  const std::size_t TripsPos = Res.Stdout.find("\"round_trips\": [");
  ASSERT_NE(TripsPos, std::string::npos);
  const std::size_t StatusPos = Res.Stdout.find("\"status\"", TripsPos);
  ASSERT_NE(StatusPos, std::string::npos);
  const std::string TripsSection =
      Res.Stdout.substr(TripsPos, StatusPos - TripsPos);
  EXPECT_NE(TripsSection.find("\"costs\": \""), std::string::npos)
      << TripsSection;
}

TEST(CliReplay, DecimalJsonNumberAndStringAreExact) {
  auto RunWithCash = [](const std::string &CashJson, const char *Name) {
    const std::filesystem::path TempPath =
        std::filesystem::temp_directory_path() / Name;
    {
      std::ofstream Out(TempPath);
      Out << R"json({
  "market": {
    "snapshot_id": "fixture-1",
    "timeframe": "D1",
    "from_date": "2024-01-09",
    "to_date": "2024-01-22",
    "initial_cash": )json"
          << CashJson << R"json(
  },
  "journal": []
})json";
    }
    const CliOutput Res = runCli("replay \"" + TempPath.string() + "\"");
    std::filesystem::remove(TempPath);
    return Res;
  };

  const CliOutput ResStr = RunWithCash("\"0.1\"", "sim_cli_dec_str.json");
  ASSERT_EQ(ResStr.ExitCode, 0) << ResStr.Stderr;
  EXPECT_NE(ResStr.Stdout.find("\"final_equity\": \"0.1\""), std::string::npos)
      << ResStr.Stdout;

  const CliOutput ResNum = RunWithCash("0.1", "sim_cli_dec_num.json");
  ASSERT_EQ(ResNum.ExitCode, 0) << ResNum.Stderr;
  EXPECT_NE(ResNum.Stdout.find("\"final_equity\": \"0.1\""), std::string::npos)
      << ResNum.Stdout;

  const CliOutput ResLarge =
      RunWithCash("9999999999999999.01", "sim_cli_dec_large.json");
  ASSERT_EQ(ResLarge.ExitCode, 0) << ResLarge.Stderr;
  EXPECT_NE(ResLarge.Stdout.find("\"final_equity\": \"9999999999999999.01\""),
            std::string::npos)
      << ResLarge.Stdout;

  const CliOutput ResLargeStr =
      RunWithCash("\"9999999999999999.01\"", "sim_cli_dec_large_str.json");
  ASSERT_EQ(ResLargeStr.ExitCode, 0) << ResLargeStr.Stderr;
  EXPECT_NE(
      ResLargeStr.Stdout.find("\"final_equity\": \"9999999999999999.01\""),
      std::string::npos)
      << ResLargeStr.Stdout;
}

TEST(CliReplay, InvalidDecimalReturnsNonZeroAndStderr) {
  auto RunBadCash = [](const std::string &CashJson, const char *Name) {
    const std::filesystem::path TempPath =
        std::filesystem::temp_directory_path() / Name;
    {
      std::ofstream Out(TempPath);
      Out << R"json({
  "market": {
    "snapshot_id": "fixture-1",
    "timeframe": "D1",
    "from_date": "2024-01-09",
    "to_date": "2024-01-22",
    "initial_cash": )json"
          << CashJson << R"json(
  },
  "journal": []
})json";
    }
    const CliOutput Res = runCli("replay \"" + TempPath.string() + "\"");
    std::filesystem::remove(TempPath);
    return Res;
  };

  for (const auto &[JsonVal, FileName] :
       std::vector<std::pair<std::string, const char *>>{
           {"\"not-a-number\"", "sim_cli_bad_dec1.json"},
           {"1.1234567890", "sim_cli_bad_dec2.json"},
           {"1e5", "sim_cli_bad_dec3.json"},
           {"true", "sim_cli_bad_dec4.json"},
       }) {
    const CliOutput Res = RunBadCash(JsonVal, FileName);
    EXPECT_NE(Res.ExitCode, 0) << JsonVal;
    EXPECT_FALSE(Res.Stderr.empty()) << JsonVal;
  }
}

} // namespace
