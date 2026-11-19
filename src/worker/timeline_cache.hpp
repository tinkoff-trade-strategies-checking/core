#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/market/instrument.hpp"
#include "core/market/timeframe.hpp"
#include "core/market/timeline.hpp"

namespace worker {

// Ключ шкалы в кэше. Снапшот данных неизменяем (INV-5), поэтому при совпадении
// всех параметров шкала детерминирована (НФТ-2) и кэшируется в памяти воркера
// до вытеснения по LRU.
struct TimelineKey {
  std::string SnapshotId;
  std::vector<core::Instrument> Instruments;
  core::Timeframe Timeframe = core::Timeframe::D1;
  std::int32_t FromYmd = 0;
  std::int32_t ToYmd = 0;
  std::int32_t WarmupBars = 0;

  friend bool operator==(const TimelineKey &, const TimelineKey &) = default;
  friend auto operator<=>(const TimelineKey &, const TimelineKey &) = default;
};

// Потокобезопасный LRU-кэш шкал времени (core::Timeline) в памяти процесса
// воркера. Позволяет обработчикам replaySession и getBars мгновенно отдавать
// готовую шкалу без повторных запросов в ClickHouse (НФТ-7: шаг плеера p95 <=
// 300 мс).
// Ограничен одновременно числом записей (MaxEntries) и суммарным числом баров
// (MaxBars, с учётом прогрева): шкала с числом баров > MaxBars не кэшируется,
// чтобы 64 длинные M1-шкалы не исчерпали память воркера.
class TimelineCache {
public:
  static constexpr std::size_t kDefaultCapacity = 64;
  // ~2 млн StepBar (~250 МБ с индексами): вмещает десятки D1/H1/M5 шкал или
  // 1-2 годовые M1-шкалы на 5 бумаг без роста памяти до гигабайт.
  static constexpr std::size_t kDefaultMaxBars = 2'000'000;

  explicit TimelineCache(std::size_t MaxEntries = kDefaultCapacity,
                         std::size_t MaxBars = kDefaultMaxBars);

  // Возвращает шкалу из кэша, если найдена (и перемещает её в MRU).
  // Если не найдена - nullptr.
  std::shared_ptr<const core::Timeline> get(const TimelineKey &Key);

  // Сохраняет шкалу в кэш. При превышении ёмкости по записям или по числу баров
  // вытесняет LRU. Шкала с totalBars() > maxBars() в кэш не помещается.
  void put(TimelineKey Key, std::shared_ptr<const core::Timeline> Timeline);

  // Число элементов в кэше.
  std::size_t size() const;

  // Ёмкость кэша по числу записей.
  std::size_t capacity() const;

  // Суммарное число баров (включая прогрев) в кэше.
  std::size_t totalBars() const;

  // Максимальное суммарное число баров в кэше.
  std::size_t maxBars() const;

  // Очистить кэш.
  void clear();

  // Кэш предыдущего торгового дня из календаря по ключу (snapshot_id,
  // FirstYmd). Значение 0 означает, что в окне поиска торговый день не найден.
  std::optional<std::int32_t>
  getPreviousTradingDay(const std::string &SnapshotId, std::int32_t FirstYmd);
  void putPreviousTradingDay(std::string SnapshotId, std::int32_t FirstYmd,
                             std::int32_t PrevYmd);

private:
  struct Entry {
    TimelineKey Key;
    std::shared_ptr<const core::Timeline> Timeline;
    std::size_t Bars = 0;
  };
  using PrevDayKey = std::pair<std::string, std::int32_t>;
  using PrevDayItem = std::pair<PrevDayKey, std::int32_t>;

  void evictWhileOverLimit();

  const std::size_t Capacity;
  const std::size_t MaxBars;
  mutable std::mutex Mutex;
  std::list<Entry> Items;
  std::map<TimelineKey, std::list<Entry>::iterator> Index;
  std::size_t TotalBars = 0;
  std::list<PrevDayItem> PrevDays;
  std::map<PrevDayKey, std::list<PrevDayItem>::iterator> PrevDayIndex;
};

} // namespace worker
