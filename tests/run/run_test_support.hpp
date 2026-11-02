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
  dsl::Program program;
  std::vector<dsl::ResolvedParam> params;
};

inline LoadedProgram loadProgram(const std::string& source) {
  dsl::ParseResult parsed = dsl::parse(source);
  if (!parsed.program) throw std::runtime_error("не разобралось: " + source);
  dsl::CheckResult checked = dsl::check(*parsed.program);
  if (checked.level == dsl::Level::Error) throw std::runtime_error("не прошло проверку: " + source);
  return LoadedProgram{std::move(*parsed.program), std::move(checked.params)};
}

inline Decimal dec(const char* text) { return Decimal::parse(text); }

inline SessionSpec specFor(std::vector<Instrument> instruments, bool naive = false) {
  SessionSpec spec;
  spec.instruments = std::move(instruments);
  spec.costs = CostConfig::defaults();
  spec.initialCash = dec("1000000");
  spec.naive = naive;
  return spec;
}

// Бумага A из fixture1: close 100, 101, 103, 106, 104, 101, 99, 102, 105, 108; лот 10, шаг 0.01.
inline std::pair<SessionSpec, std::shared_ptr<const Timeline>> fixtureA() {
  auto f = fixture1();
  std::vector<Instrument> instruments{f.instruments[0]};
  auto timeline = std::make_shared<const Timeline>(
      Timeline::load(f.bars, f.calendar, f.snapshotId, instruments, Timeframe::D1, 20240109, 20240122));
  return {specFor(instruments), timeline};
}

inline Instrument syntheticInstrument() { return Instrument{"A", "SYN", 1, dec("0.01")}; }

// Дневные бары одной бумаги по ценам закрытия: open - предыдущий close (первый - свой close), high/low - вокруг.
// openOverrides[i], если непусто, задаёт open бара i (для гэпов).
inline std::pair<SessionSpec, std::shared_ptr<const Timeline>> syntheticTimeline(
    const std::vector<std::string>& closes, const std::vector<std::string>& opens = {}) {
  constexpr std::int64_t kDayMs = 86'400'000;
  constexpr std::int64_t kStart = 1'704'747'600'000;  // 2024-01-09 00:00 МСК
  std::vector<Bar> bars;
  Decimal previous = dec(closes.front().c_str());
  for (std::size_t i = 0; i < closes.size(); ++i) {
    Bar b;
    b.tsOpenMs = kStart + static_cast<std::int64_t>(i) * kDayMs;
    b.availableAtMs = b.tsOpenMs + kDayMs;
    b.close = dec(closes[i].c_str());
    b.open = opens.empty() ? previous : dec(opens[i].c_str());
    b.high = (b.open > b.close ? b.open : b.close) + dec("0.5");
    b.low = (b.open < b.close ? b.open : b.close) - dec("0.5");
    b.volume = 1'000'000;
    b.value = b.close.times(b.volume);
    bars.push_back(b);
    previous = b.close;
  }
  const Instrument instrument = syntheticInstrument();
  auto timeline = std::make_shared<const Timeline>(Timeline::build({instrument}, {bars}, {}));
  return {specFor({instrument}), timeline};
}

// Детерминированное случайное блуждание (LCG, без часов и random_device).
inline std::vector<std::string> randomWalk(std::size_t n, std::uint64_t seed = 12345) {
  std::vector<std::string> closes;
  std::int64_t price = 10000;  // копейки
  std::uint64_t state = seed;
  for (std::size_t i = 0; i < n; ++i) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::int64_t move = static_cast<std::int64_t>((state >> 33) % 201) - 100;  // -1.00 .. +1.00 ₽
    price = std::max<std::int64_t>(price + move, 1000);
    closes.push_back(std::to_string(price / 100) + "." + (price % 100 < 10 ? "0" : "") + std::to_string(price % 100));
  }
  return closes;
}

inline bool sameFill(const Fill& a, const Fill& b) {
  return a.orderId == b.orderId && a.step == b.step && a.instrument == b.instrument && a.side == b.side &&
         a.lots == b.lots && a.price == b.price && a.basePrice == b.basePrice && a.value == b.value &&
         a.commission == b.commission && a.fee == b.fee && a.spreadCost == b.spreadCost &&
         a.slippageCost == b.slippageCost && a.stopAndTakeSameBar == b.stopAndTakeSameBar;
}

}  // namespace core::testing
