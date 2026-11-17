#include <algorithm>
#include <stdexcept>
#include <utility>

#include "core/market/bar_source.hpp"
#include "core/market/calendar_source.hpp"

namespace core {

void InMemoryCalendarSource::add(const std::string &snapshotId,
                                 std::vector<TradingDay> days) {
  std::stable_sort(days.begin(), days.end(),
                   [](const TradingDay &a, const TradingDay &b) {
                     return a.yyyymmdd < b.yyyymmdd;
                   });
  for (std::size_t i = 1; i < days.size(); ++i) {
    if (days[i].yyyymmdd == days[i - 1].yyyymmdd) {
      throw std::invalid_argument("InMemoryCalendarSource: дубликат даты");
    }
  }
  days_[snapshotId] = std::move(days);
}

std::vector<TradingDay>
InMemoryCalendarSource::tradingDays(const std::string &snapshotId,
                                    std::int32_t fromYmd, std::int32_t toYmd) {
  const auto it = days_.find(snapshotId);
  if (it == days_.end())
    throw SnapshotNotFound("календарь версии данных не найден: " + snapshotId);
  std::vector<TradingDay> out;
  for (const TradingDay &day : it->second) {
    if (day.yyyymmdd >= fromYmd && day.yyyymmdd <= toYmd)
      out.push_back(day);
  }
  return out;
}

} // namespace core
