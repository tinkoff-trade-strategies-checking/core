#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Календарные и московские даты без системной базы tz (INV-4).
// Переходы МСК: UTC+3 с 2014-10-26, UTC+4 в 2011-03-27…2014-10-26,
// сезонный перевод РФ ранее (поддерживаемый диапазон — с 1996 года).

namespace core {

inline constexpr std::int64_t kDayMs = 86'400'000;
inline constexpr std::int64_t kHourMs = 3'600'000;
inline constexpr std::int64_t kMoscowUtc3Ms = 3 * kHourMs;
inline constexpr std::int64_t kMoscowUtc4Ms = 4 * kHourMs;

// Дней от 1970-01-01 до даты (григорианский календарь, алгоритм Говарда
// Хиннанта).
constexpr std::int64_t daysFromCivil(std::int32_t Y, std::int32_t M,
                                     std::int32_t D) {
  Y -= M <= 2 ? 1 : 0;
  const std::int64_t Era = (Y >= 0 ? Y : Y - 399) / 400;
  const std::int64_t Yoe = Y - Era * 400;
  const std::int64_t Doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
  const std::int64_t Doe = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;
  return Era * 146097 + Doe - 719468;
}

// Дней от 1970-01-01 до даты в формате yyyymmdd.
constexpr std::int64_t ymdToDays(std::int32_t Ymd) {
  return daysFromCivil(Ymd / 10000, Ymd / 100 % 100, Ymd % 100);
}

// Дата в формате yyyymmdd по числу дней от 1970-01-01.
constexpr std::int32_t civilFromDays(std::int64_t Z) {
  Z += 719468;
  const std::int64_t Era = (Z >= 0 ? Z : Z - 146096) / 146097;
  const std::int64_t Doe = Z - Era * 146097;
  const std::int64_t Yoe =
      (Doe - Doe / 1460 + Doe / 36524 - Doe / 146096) / 365;
  const std::int64_t Doy = Doe - (365 * Yoe + Yoe / 4 - Yoe / 100);
  const std::int64_t Mp = (5 * Doy + 2) / 153;
  const std::int64_t D = Doy - (153 * Mp + 2) / 5 + 1;
  const std::int64_t M = Mp < 10 ? Mp + 3 : Mp - 9;
  const std::int64_t Y = Yoe + Era * 400 + (M <= 2 ? 1 : 0);
  return static_cast<std::int32_t>(Y * 10000 + M * 100 + D);
}

// Смещение МСК относительно UTC в момент TsMs (мс) без системной базы tz.
std::int64_t moscowOffsetMs(std::int64_t TsMs);

// Московская дата момента TsMs (UTC, мс) в виде yyyymmdd.
std::int32_t moscowYmd(std::int64_t TsMs);

// Начало московских суток даты Ymd в UTC, мс.
std::int64_t moscowDayStartMs(std::int32_t Ymd);

// "yyyy-mm-dd" -> yyyymmdd; неверная строка -> std::invalid_argument.
std::int32_t parseIsoDate(std::string_view Text);

// yyyymmdd -> "yyyy-mm-dd".
std::string formatIsoDate(std::int32_t Yyyymmdd);

} // namespace core
