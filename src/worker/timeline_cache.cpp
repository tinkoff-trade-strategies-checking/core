#include "timeline_cache.hpp"

#include <utility>

namespace worker {

TimelineCache::TimelineCache(std::size_t MaxEntries, std::size_t MaxBarsLimit)
    : Capacity(MaxEntries > 0 ? MaxEntries : 1),
      MaxBars(MaxBarsLimit > 0 ? MaxBarsLimit : 1) {}

std::shared_ptr<const core::Timeline>
TimelineCache::get(const TimelineKey &Key) {
  std::lock_guard<std::mutex> Lock(Mutex);
  const auto It = Index.find(Key);
  if (It == Index.end()) {
    return nullptr;
  }
  Items.splice(Items.begin(), Items, It->second);
  return It->second->Timeline;
}

void TimelineCache::evictWhileOverLimit() {
  while (!Items.empty() && (Items.size() > Capacity || TotalBars > MaxBars)) {
    const auto Last = --Items.end();
    TotalBars -= Last->Bars;
    Index.erase(Last->Key);
    Items.pop_back();
  }
}

void TimelineCache::put(TimelineKey Key,
                        std::shared_ptr<const core::Timeline> Timeline) {
  if (!Timeline) {
    return;
  }
  const std::size_t Bars = Timeline->totalBars();
  std::lock_guard<std::mutex> Lock(Mutex);
  const auto It = Index.find(Key);
  if (Bars > MaxBars) {
    if (It != Index.end()) {
      TotalBars -= It->second->Bars;
      Items.erase(It->second);
      Index.erase(It);
    }
    return;
  }
  if (It != Index.end()) {
    TotalBars -= It->second->Bars;
    It->second->Timeline = std::move(Timeline);
    It->second->Bars = Bars;
    TotalBars += Bars;
    Items.splice(Items.begin(), Items, It->second);
    evictWhileOverLimit();
    return;
  }

  // Сначала вставка (может бросить), потом вытеснение: при исключении кэш
  // остаётся согласованным (Items и Index одного размера).
  Items.push_front(Entry{Key, std::move(Timeline), Bars});
  try {
    Index.emplace(std::move(Key), Items.begin());
  } catch (...) {
    Items.pop_front();
    throw;
  }
  TotalBars += Bars;
  evictWhileOverLimit();
}

std::size_t TimelineCache::size() const {
  std::lock_guard<std::mutex> Lock(Mutex);
  return Items.size();
}

std::size_t TimelineCache::capacity() const { return Capacity; }

std::size_t TimelineCache::totalBars() const {
  std::lock_guard<std::mutex> Lock(Mutex);
  return TotalBars;
}

std::size_t TimelineCache::maxBars() const { return MaxBars; }

void TimelineCache::clear() {
  std::lock_guard<std::mutex> Lock(Mutex);
  Items.clear();
  Index.clear();
  TotalBars = 0;
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
