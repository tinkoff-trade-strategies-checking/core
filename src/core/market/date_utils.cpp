#include "core/market/date_utils.hpp"

#include <cctype>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace core {

namespace {

std::int64_t floorDiv(std::int64_t A, std::int64_t B) {
  std::int64_t Q = A / B;
  if ((A % B != 0) && ((A < 0) != (B < 0)))
    --Q;
  return Q;
}

// Номер дня последнего воскресенья месяца (для марта и октября — 31 день).
std::int64_t lastSundayDays(std::int32_t Year, std::int32_t Month) {
  const std::int64_t Last = daysFromCivil(Year, Month, 31);
  const std::int64_t Wday = ((Last + 4) % 7 + 7) % 7; // 0 = воскресенье
  return Last - Wday;
}

} // namespace

std::int64_t moscowOffsetMs(std::int64_t TsMs) {
  constexpr std::int64_t kOct2014SwitchMs =
      daysFromCivil(2014, 10, 25) * kDayMs + 22 * kHourMs;
  if (TsMs >= kOct2014SwitchMs)
    return kMoscowUtc3Ms;

  constexpr std::int64_t kMar2011SwitchMs =
      daysFromCivil(2011, 3, 26) * kDayMs + 23 * kHourMs;
  if (TsMs >= kMar2011SwitchMs)
    return kMoscowUtc4Ms;

  const std::int32_t ApproxYmd = civilFromDays(floorDiv(TsMs, kDayMs));
  const std::int32_t Year = ApproxYmd / 10000;
  const std::int64_t SpringSwitchMs =
      (lastSundayDays(Year, 3) - 1) * kDayMs + 23 * kHourMs;
  const std::int64_t AutumnSwitchMs =
      (lastSundayDays(Year, 10) - 1) * kDayMs + 23 * kHourMs;
  if (TsMs >= SpringSwitchMs && TsMs < AutumnSwitchMs)
    return kMoscowUtc4Ms;
  return kMoscowUtc3Ms;
}

std::int32_t moscowYmd(std::int64_t TsMs) {
  return civilFromDays(floorDiv(TsMs + moscowOffsetMs(TsMs), kDayMs));
}

std::int64_t moscowDayStartMs(std::int32_t Ymd) {
  const std::int64_t Days = ymdToDays(Ymd);
  return Days * kDayMs - moscowOffsetMs(Days * kDayMs - kMoscowUtc3Ms);
}

std::int32_t parseIsoDate(std::string_view Text) {
  bool Ok = Text.size() == 10 && Text[4] == '-' && Text[7] == '-';
  for (std::size_t I = 0; Ok && I < Text.size(); ++I) {
    if (I != 4 && I != 7 && !std::isdigit(static_cast<unsigned char>(Text[I])))
      Ok = false;
  }
  if (!Ok) {
    throw std::invalid_argument("неверная дата, нужен формат гггг-мм-дд: " +
                                std::string(Text));
  }
  const std::int32_t Year = std::stoi(std::string(Text.substr(0, 4)));
  const std::int32_t Month = std::stoi(std::string(Text.substr(5, 2)));
  const std::int32_t Day = std::stoi(std::string(Text.substr(8, 2)));
  if (Month < 1 || Month > 12 || Day < 1 || Day > 31)
    throw std::invalid_argument("неверная дата: " + std::string(Text));
  return Year * 10000 + Month * 100 + Day;
}

std::string formatIsoDate(std::int32_t Yyyymmdd) {
  std::string Out = std::to_string(Yyyymmdd);
  Out.insert(6, "-");
  Out.insert(4, "-");
  return Out;
}

} // namespace core
