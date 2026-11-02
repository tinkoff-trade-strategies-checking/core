#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <utility>

#include "core/run/backtest.hpp"

// Проверки во время исполнения (dsl.md, «Чтение будущего», таблица «Во время исполнения»).

namespace core::detail {

namespace {

// Пороги из dsl.md.
constexpr std::int64_t kGapPercent = 2;            // |open(t+1) / close(t) - 1| >= 2 %
constexpr std::int64_t kGapMinGuessed = 5;         // угаданных гэпов >= 5
constexpr std::int64_t kGapMinSharePercent = 80;   // и >= 80 % гэпов с исполненной заявкой перед ними
constexpr std::int64_t kEvenMinTrips = 30;         // закрытых сделок >= 30
constexpr std::int64_t kEvenMinWinPercent = 90;    // доля прибыльных > 90 %

const Order* findOrder(const std::vector<Order>& orders, std::int64_t id) {
  const auto it = std::lower_bound(orders.begin(), orders.end(), id,
                                   [](const Order& o, std::int64_t value) { return o.id < value; });
  return it != orders.end() && it->id == id ? &*it : nullptr;
}

// |open - close| / close >= 2 % без double: |open - close| * 100 >= 2 * close.
bool isGap(const Decimal& open, const Decimal& prevClose) {
  if (prevClose.sign() <= 0) return false;
  return (open - prevClose).abs().times(100) >= prevClose.times(kGapPercent);
}

// Находка W-RT01: гэпы, перед которыми стратегия создала исполненную заявку, и из них угаданные.
// Знаменатель - гэпы, перед которыми заявка подана на баре t и исполнена на баре t + 1 (заявки, поданные раньше и
// исполненные на t + 1, например лимитные, в него не входят): так задумано, `dsl.md` уточняет формулировку (задача 54).
void checkGuessedGaps(const Timeline& timeline, const std::vector<Order>& orders, const std::vector<Fill>& fills,
                      std::vector<Finding>& out) {
  struct Gap {
    bool guessed = false;
    std::int64_t step = 0;  // шаг бара t + 1
  };
  std::map<std::pair<int, std::int64_t>, Gap> gaps;  // (бумага, шаг бара t)
  for (const Fill& fill : fills) {
    const Order* order = findOrder(orders, fill.orderId);
    const StepBar* bar = timeline.barOf(fill.instrument, fill.step);
    if (order == nullptr || bar == nullptr || fill.step < 1) continue;
    // Бар t - предыдущий бар бумаги; заявка должна быть подана на нём.
    const StepBar* prev = timeline.lastBarOf(fill.instrument, fill.step - 1);
    if (prev == nullptr || order->placedStep != prev->step) continue;
    if (!isGap(bar->open, prev->close)) continue;
    Gap& gap = gaps[{fill.instrument, prev->step}];
    gap.step = fill.step;
    const bool up = bar->open > prev->close;
    const bool atOpen = fill.basePrice == bar->open;
    if (atOpen && ((fill.side == Side::Buy && up) || (fill.side == Side::Sell && !up))) gap.guessed = true;
  }
  std::int64_t guessed = 0, lastStep = 0;
  for (const auto& [key, gap] : gaps) {
    if (gap.guessed) ++guessed;
    lastStep = std::max(lastStep, gap.step);
  }
  const auto total = static_cast<std::int64_t>(gaps.size());
  if (guessed >= kGapMinGuessed && guessed * 100 >= total * kGapMinSharePercent) {
    out.push_back(Finding{"W-RT01", dsl::Level::Warning,
                          "Стратегия угадала гэпы: " + std::to_string(guessed) + " из " + std::to_string(total) +
                              " (покупка перед гэпом вверх, продажа перед гэпом вниз)",
                          lastStep});
  }
}

// Находка W-RT02: слишком ровный результат.
void checkTooEven(const std::vector<RoundTrip>& roundTrips, std::vector<Finding>& out) {
  const auto total = static_cast<std::int64_t>(roundTrips.size());
  if (total < kEvenMinTrips) return;
  std::int64_t wins = 0, lastStep = 0;
  for (const RoundTrip& trip : roundTrips) {
    if (trip.netPnl.sign() > 0) ++wins;
    lastStep = std::max(lastStep, trip.exitStep);
  }
  if (wins * 100 > total * kEvenMinWinPercent) {
    out.push_back(Finding{"W-RT02", dsl::Level::Warning,
                          "Слишком ровный результат: прибыльных закрытых сделок " + std::to_string(wins) + " из " +
                              std::to_string(total),
                          lastStep});
  }
}

}  // namespace

std::vector<Finding> runtimeFindings(const Timeline& timeline, const std::vector<Order>& orders,
                                     const std::vector<Fill>& fills, const std::vector<RoundTrip>& roundTrips) {
  std::vector<Finding> out;
  checkGuessedGaps(timeline, orders, fills, out);
  checkTooEven(roundTrips, out);
  return out;
}

Finding integrityFinding(const IntegrityViolation& violation, std::int64_t step) {
  return Finding{"E-RT01", dsl::Level::Error,
                 std::string("Нарушена целостность времени в движке: ") + violation.what() + ". Прогон остановлен", step};
}

dsl::Level worstLevel(dsl::Level staticLevel, const std::vector<Finding>& findings) {
  dsl::Level level = staticLevel;
  for (const Finding& f : findings) level = std::max(level, f.level);
  return level;
}

}  // namespace core::detail
