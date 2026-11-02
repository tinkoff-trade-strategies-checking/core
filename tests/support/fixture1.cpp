#include "support/fixture1.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

namespace core::testing {

namespace {

constexpr std::int64_t kDayMs = 86'400'000;
constexpr std::int64_t kMoscowOffsetMs = 3 * 3'600'000;

// Дней от 1970-01-01 до даты (григорианский календарь).
std::int64_t daysFromCivil(std::int32_t y, std::int32_t m, std::int32_t d) {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const std::int64_t yoe = y - era * 400;
  const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Начало московских суток в UTC, мс.
std::int64_t moscowDayStartMs(std::int32_t ymd) {
  return daysFromCivil(ymd / 10000, ymd / 100 % 100, ymd % 100) * kDayMs - kMoscowOffsetMs;
}

}  // namespace

Fixture fixture1() {
  const std::string snapshot = "fixture-1";
  // Торговые дни по порядку (12 января - пятница, 15 - понедельник).
  const std::int32_t tradingDays[] = {20240109, 20240110, 20240111, 20240112, 20240115,
                                      20240116, 20240117, 20240118, 20240119, 20240122};
  const char* const closesA[] = {"100", "101", "103", "106", "104", "101", "99", "102", "105", "108"};

  Fixture f;
  f.snapshotId = snapshot;
  f.instruments = {Instrument{"A", "FIXA", 10, Decimal::parse("0.01")},
                   Instrument{"B", "FIXB", 10000, Decimal::parse("0.000005")}};
  f.bars.addSecurity(snapshot, Security{"FIXA", "Финансы", 10, Decimal::parse("0.01")});
  f.bars.addSecurity(snapshot, Security{"FIXB", "Энергетика", 10000, Decimal::parse("0.000005")});

  std::vector<Bar> a;
  std::vector<Bar> b;
  Decimal previousClose = Decimal::parse("100");
  for (int i = 0; i < 10; ++i) {
    const std::int64_t open = moscowDayStartMs(tradingDays[i]);
    const Decimal close = Decimal::parse(closesA[i]);
    Bar bar;
    bar.tsOpenMs = open;
    bar.availableAtMs = open + kDayMs;
    bar.open = previousClose;
    bar.high = std::max(previousClose, close) + Decimal::one();
    bar.low = std::min(previousClose, close) - Decimal::one();
    bar.close = close;
    bar.volume = 100'000;
    bar.value = close.times(bar.volume);
    a.push_back(bar);
    previousClose = close;

    if (tradingDays[i] == 20240112) continue;  // у B нет бара 12 января
    Bar other;
    other.tsOpenMs = open;
    other.availableAtMs = open + kDayMs;
    other.open = other.high = other.low = other.close = Decimal::parse("0.0200");
    other.volume = 1'000'000'000;
    other.value = other.close.times(other.volume);
    b.push_back(other);
  }
  f.bars.addSeries(snapshot, "FIXA", 86400, std::move(a));
  f.bars.addSeries(snapshot, "FIXB", 86400, std::move(b));

  // Календарь 2024-01-08…22: торговые дни - из списка выше, остальные (в т. ч. 8 января) - нет.
  std::vector<TradingDay> calendar;
  for (std::int32_t day = 20240108; day <= 20240122; ++day) {
    const bool trading = std::find(std::begin(tradingDays), std::end(tradingDays), day) != std::end(tradingDays);
    calendar.push_back(TradingDay{day, trading});
  }
  f.calendar.add(snapshot, std::move(calendar));
  return f;
}

}  // namespace core::testing
