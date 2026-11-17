#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/dsl/checker.hpp"
#include "core/dsl/parser.hpp"
#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "support/fixture1.hpp"

// Общие помощники тестов прогона стратегии (tests/run).

namespace core::testing {

struct LoadedProgram {
  dsl::Program Program;
  std::vector<dsl::ResolvedParam> Params;
};

inline LoadedProgram loadProgram(const std::string &Source) {
  dsl::ParseResult Parsed = dsl::parse(Source);
  if (!Parsed.Ast)
    throw std::runtime_error("не разобралось: " + Source);
  dsl::CheckResult Checked = dsl::check(*Parsed.Ast);
  if (Checked.Severity == dsl::Level::Error)
    throw std::runtime_error("не прошло проверку: " + Source);
  return LoadedProgram{std::move(*Parsed.Ast), std::move(Checked.Params)};
}

inline Decimal dec(const char *Text) { return Decimal::parse(Text); }

inline SessionSpec specFor(std::vector<Instrument> Instruments,
                           bool Naive = false) {
  SessionSpec Spec;
  Spec.Instruments = std::move(Instruments);
  Spec.Costs = CostConfig::defaults();
  Spec.InitialCash = dec("1000000");
  Spec.Naive = Naive;
  return Spec;
}

// Бумага A из fixture1: close 100, 101, 103, 106, 104, 101, 99, 102, 105, 108;
// лот 10, шаг 0.01.
inline std::pair<SessionSpec, std::shared_ptr<const Timeline>> fixtureA() {
  auto F = fixture1();
  std::vector<Instrument> Instruments{F.Instruments[0]};
  auto Line = std::make_shared<const Timeline>(
      Timeline::load(F.Bars, F.Calendar, F.SnapshotId, Instruments,
                     Timeframe::D1, 20240109, 20240122));
  return {specFor(Instruments), Line};
}

inline Instrument syntheticInstrument() {
  return Instrument{"A", "SYN", 1, dec("0.01")};
}

// Дневные бары одной бумаги по ценам закрытия: open - предыдущий close (первый
// - свой close), high/low - вокруг. openOverrides[i], если непусто, задаёт open
// бара i (для гэпов).
inline std::pair<SessionSpec, std::shared_ptr<const Timeline>>
syntheticTimeline(const std::vector<std::string> &Closes,
                  const std::vector<std::string> &Opens = {}) {
  constexpr std::int64_t kDayMs = 86'400'000;
  constexpr std::int64_t kStart = 1'704'747'600'000; // 2024-01-09 00:00 МСК
  std::vector<Bar> Bars;
  Decimal Previous = dec(Closes.front().c_str());
  for (std::size_t I = 0; I < Closes.size(); ++I) {
    Bar B;
    B.TsOpenMs = kStart + static_cast<std::int64_t>(I) * kDayMs;
    B.AvailableAtMs = B.TsOpenMs + kDayMs;
    B.Close = dec(Closes[I].c_str());
    B.Open = Opens.empty() ? Previous : dec(Opens[I].c_str());
    B.High = (B.Open > B.Close ? B.Open : B.Close) + dec("0.5");
    B.Low = (B.Open < B.Close ? B.Open : B.Close) - dec("0.5");
    B.Volume = 1'000'000;
    B.Value = B.Close.times(B.Volume);
    Bars.push_back(B);
    Previous = B.Close;
  }
  const Instrument Instr = syntheticInstrument();
  auto Line =
      std::make_shared<const Timeline>(Timeline::build({Instr}, {Bars}, {}));
  return {specFor({Instr}), Line};
}

// Детерминированное случайное блуждание (LCG, без часов и random_device).
inline std::vector<std::string> randomWalk(std::size_t N,
                                           std::uint64_t Seed = 12345) {
  std::vector<std::string> Closes;
  std::int64_t Price = 10000; // копейки
  std::uint64_t State = Seed;
  for (std::size_t I = 0; I < N; ++I) {
    State = State * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::int64_t Move = static_cast<std::int64_t>((State >> 33) % 201) -
                              100; // -1.00 .. +1.00 ₽
    Price = std::max<std::int64_t>(Price + Move, 1000);
    Closes.push_back(std::to_string(Price / 100) + "." +
                     (Price % 100 < 10 ? "0" : "") +
                     std::to_string(Price % 100));
  }
  return Closes;
}

inline bool sameFill(const Fill &A, const Fill &B) {
  return A.OrderId == B.OrderId && A.Step == B.Step &&
         A.Instrument == B.Instrument && A.OrderSide == B.OrderSide &&
         A.Lots == B.Lots && A.Price == B.Price && A.BasePrice == B.BasePrice &&
         A.Value == B.Value && A.Commission == B.Commission && A.Fee == B.Fee &&
         A.SpreadCost == B.SpreadCost && A.SlippageCost == B.SlippageCost &&
         A.StopAndTakeSameBar == B.StopAndTakeSameBar;
}

} // namespace core::testing
