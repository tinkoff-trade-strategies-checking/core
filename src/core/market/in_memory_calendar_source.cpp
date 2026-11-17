#include <algorithm>
#include <stdexcept>
#include <utility>

#include "core/market/bar_source.hpp"
#include "core/market/calendar_source.hpp"

namespace core {

void InMemoryCalendarSource::add(const std::string &SnapshotId,
                                 std::vector<TradingDay> SnapshotDays) {
  std::stable_sort(SnapshotDays.begin(), SnapshotDays.end(),
                   [](const TradingDay &A, const TradingDay &B) {
                     return A.Yyyymmdd < B.Yyyymmdd;
                   });
  for (std::size_t I = 1; I < SnapshotDays.size(); ++I) {
    if (SnapshotDays[I].Yyyymmdd == SnapshotDays[I - 1].Yyyymmdd) {
      throw std::invalid_argument("InMemoryCalendarSource: дубликат даты");
    }
  }
  Days[SnapshotId] = std::move(SnapshotDays);
}

std::vector<TradingDay>
InMemoryCalendarSource::tradingDays(const std::string &SnapshotId,
                                    std::int32_t FromYmd, std::int32_t ToYmd) {
  const auto It = Days.find(SnapshotId);
  if (It == Days.end())
    throw SnapshotNotFound("календарь версии данных не найден: " + SnapshotId);
  std::vector<TradingDay> Out;
  for (const TradingDay &Day : It->second) {
    if (Day.Yyyymmdd >= FromYmd && Day.Yyyymmdd <= ToYmd)
      Out.push_back(Day);
  }
  return Out;
}

} // namespace core
