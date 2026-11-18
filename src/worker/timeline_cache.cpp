#include "timeline_cache.hpp"

#include <utility>

namespace worker {

TimelineCache::TimelineCache(std::size_t MaxEntries)
    : Capacity(MaxEntries > 0 ? MaxEntries : 1) {}

std::shared_ptr<const core::Timeline>
TimelineCache::get(const TimelineKey &Key) {
  std::lock_guard<std::mutex> Lock(Mutex);
  const auto It = Index.find(Key);
  if (It == Index.end()) {
    return nullptr;
  }
  Items.splice(Items.begin(), Items, It->second);
  return It->second->second;
}

void TimelineCache::put(TimelineKey Key,
                        std::shared_ptr<const core::Timeline> Timeline) {
  if (!Timeline) {
    return;
  }
  std::lock_guard<std::mutex> Lock(Mutex);
  const auto It = Index.find(Key);
  if (It != Index.end()) {
    It->second->second = std::move(Timeline);
    Items.splice(Items.begin(), Items, It->second);
    return;
  }

  // Сначала вставка (может бросить), потом вытеснение: при исключении кэш
  // остаётся согласованным (Items и Index одного размера).
  Items.emplace_front(Key, std::move(Timeline));
  try {
    Index.emplace(std::move(Key), Items.begin());
  } catch (...) {
    Items.pop_front();
    throw;
  }
  if (Items.size() > Capacity) {
    const auto Last = --Items.end();
    Index.erase(Last->first);
    Items.pop_back();
  }
}

std::size_t TimelineCache::size() const {
  std::lock_guard<std::mutex> Lock(Mutex);
  return Items.size();
}

std::size_t TimelineCache::capacity() const { return Capacity; }

void TimelineCache::clear() {
  std::lock_guard<std::mutex> Lock(Mutex);
  Items.clear();
  Index.clear();
  PrevDays.clear();
  PrevDayIndex.clear();
}

std::optional<std::int32_t>
TimelineCache::getPreviousTradingDay(const std::string &SnapshotId,
                                     std::int32_t FirstYmd) {
  std::lock_guard<std::mutex> Lock(Mutex);
  const auto It = PrevDayIndex.find(PrevDayKey{SnapshotId, FirstYmd});
  if (It == PrevDayIndex.end()) {
    return std::nullopt;
  }
  PrevDays.splice(PrevDays.begin(), PrevDays, It->second);
  return It->second->second;
}

void TimelineCache::putPreviousTradingDay(std::string SnapshotId,
                                          std::int32_t FirstYmd,
                                          std::int32_t PrevYmd) {
  std::lock_guard<std::mutex> Lock(Mutex);
  PrevDayKey Key{std::move(SnapshotId), FirstYmd};
  const auto It = PrevDayIndex.find(Key);
  if (It != PrevDayIndex.end()) {
    It->second->second = PrevYmd;
    PrevDays.splice(PrevDays.begin(), PrevDays, It->second);
    return;
  }

  PrevDays.emplace_front(Key, PrevYmd);
  try {
    PrevDayIndex.emplace(std::move(Key), PrevDays.begin());
  } catch (...) {
    PrevDays.pop_front();
    throw;
  }
  if (PrevDays.size() > Capacity) {
    const auto Last = --PrevDays.end();
    PrevDayIndex.erase(Last->first);
    PrevDays.pop_back();
  }
}

} // namespace worker
