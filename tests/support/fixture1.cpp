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
std::int64_t daysFromCivil(std::int32_t Y, std::int32_t M, std::int32_t D) {
  Y -= M <= 2 ? 1 : 0;
  const std::int64_t Era = (Y >= 0 ? Y : Y - 399) / 400;
  const std::int64_t Yoe = Y - Era * 400;
  const std::int64_t Doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
  const std::int64_t Doe = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;
  return Era * 146097 + Doe - 719468;
}

// Начало московских суток в UTC, мс.
std::int64_t moscowDayStartMs(std::int32_t Ymd) {
  return daysFromCivil(Ymd / 10000, Ymd / 100 % 100, Ymd % 100) * kDayMs -
         kMoscowOffsetMs;
}

} // namespace

Fixture fixture1() {
  const std::string Snapshot = "fixture-1";
  // Торговые дни по порядку (12 января - пятница, 15 - понедельник).
  const std::int32_t TradingDays[] = {20240109, 20240110, 20240111, 20240112,
                                      20240115, 20240116, 20240117, 20240118,
                                      20240119, 20240122};
  const char *const ClosesA[] = {"100", "101", "103", "106", "104",
                                 "101", "99",  "102", "105", "108"};

  Fixture F;
  F.SnapshotId = Snapshot;
  F.Instruments = {Instrument{"A", "FIXA", 10, Decimal::parse("0.01")},
                   Instrument{"B", "FIXB", 10000, Decimal::parse("0.000005")}};
  F.Bars.addSecurity(Snapshot,
                     Security{"FIXA", "Финансы", 10, Decimal::parse("0.01")});
  F.Bars.addSecurity(Snapshot, Security{"FIXB", "Энергетика", 10000,
                                        Decimal::parse("0.000005")});

  std::vector<Bar> A;
  std::vector<Bar> B;
  Decimal PreviousClose = Decimal::parse("100");
  for (int I = 0; I < 10; ++I) {
    const std::int64_t Open = moscowDayStartMs(TradingDays[I]);
    const Decimal Close = Decimal::parse(ClosesA[I]);
    Bar BarA;
    BarA.TsOpenMs = Open;
    BarA.AvailableAtMs = Open + kDayMs;
    BarA.Open = PreviousClose;
    BarA.High = std::max(PreviousClose, Close) + Decimal::one();
    BarA.Low = std::min(PreviousClose, Close) - Decimal::one();
    BarA.Close = Close;
    BarA.Volume = 100'000;
    BarA.Value = Close.times(BarA.Volume);
    A.push_back(BarA);
    PreviousClose = Close;

    if (TradingDays[I] == 20240112)
      continue; // у B нет бара 12 января
    Bar Other;
    Other.TsOpenMs = Open;
    Other.AvailableAtMs = Open + kDayMs;
    Other.Open = Other.High = Other.Low = Other.Close =
        Decimal::parse("0.0200");
    Other.Volume = 1'000'000'000;
    Other.Value = Other.Close.times(Other.Volume);
    B.push_back(Other);
  }
  F.Bars.addSeries(Snapshot, "FIXA", 86400, std::move(A));
  F.Bars.addSeries(Snapshot, "FIXB", 86400, std::move(B));

  // Календарь 2024-01-08…22: торговые дни - из списка выше, остальные (в т. ч.
  // 8 января) - нет.
  std::vector<TradingDay> Calendar;
  for (std::int32_t Day = 20240108; Day <= 20240122; ++Day) {
    const bool Trading =
        std::find(std::begin(TradingDays), std::end(TradingDays), Day) !=
        std::end(TradingDays);
    Calendar.push_back(TradingDay{Day, Trading});
  }
  F.Calendar.add(Snapshot, std::move(Calendar));
  return F;
}

} // namespace core::testing
