#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "clickhouse/clickhouse_client.hpp"
#include "core/market/bar_source.hpp"

namespace worker {

// Чтение баров из ClickHouse (ФТ-3, ФТ-5); таблицы описаны в
// core/AGENTS.md. Правила:
//  - читаются только версии данных из `snapshots_ready` (иначе
//    SnapshotNotFound), INV-5;
//  - неизменяемые метаданные снапшота (готовность, справочник securities,
//    хранимые интервалы в candles) кэшируются по snapshot_id (INV-5);
//  - серии баров кэшируются в ограниченном потокобезопасном LRU-кэше;
//  - если в версии для (secid, interval) есть свечи в `candles` — берутся они;
//    иначе свечи агрегируются из минутных свечей (`interval_sec = 60`)
//    средствами ClickHouse (интервал 60...86400 с, кратный 60 с); выбор
//    делается по наличию, а не по диапазону: один запрос не смешивает два
//    источника;
//  - INV-1: в результат попадают только бары, уже известные к toMs
//    (available_at <= toMs);
//  - порядок строго по ts_open, поэтому результат детерминирован (INV-4);
//  - цены разбираются из текста через Decimal::parse, double не используется
//    (INV-6);
//  - availableAtMs — `available_at` из данных (для агрегированных свечей —
//    `bar_ts_open + interval_ms`, INV-1);
//  - loadBatch загружает промахи кэша одним запросом (`secid IN (...)`) на
//    каждый режим чтения (хранимые свечи / агрегация).
class ClickHouseBarSource final : public core::BarSource {
public:
  static constexpr std::size_t kDefaultBarCacheEntries = 512;
  // ~500 тыс. core::Bar (~64 МБ) — вмещает десятки мультибумажных окон без
  // неограниченного роста памяти воркера.
  static constexpr std::size_t kDefaultBarCacheMaxBars = 500'000;

  explicit ClickHouseBarSource(
      std::unique_ptr<ClickHouseClient> Client,
      std::size_t MaxCacheEntries = kDefaultBarCacheEntries,
      std::size_t MaxCacheBars = kDefaultBarCacheMaxBars);
  // Боевая сборка: libcurl-клиент по настройкам.
  explicit ClickHouseBarSource(
      const ClickHouseConfig &Config,
      std::size_t MaxCacheEntries = kDefaultBarCacheEntries,
      std::size_t MaxCacheBars = kDefaultBarCacheMaxBars);

  std::vector<core::Bar> load(const core::BarRequest &Request) override;
  std::vector<std::vector<core::Bar>>
  loadBatch(const core::BarBatchRequest &Request) override;
  core::Security security(const std::string &SnapshotId,
                          const std::string &Secid) override;

private:
  struct SnapshotSecurities {
    std::unordered_map<std::string, core::Security> BySecid;
    std::unordered_set<std::string> AmbiguousSecids;
  };

  using StoredIntervalsMap =
      std::unordered_map<std::string, std::unordered_set<std::int32_t>>;

  struct BarCacheKey {
    std::string SnapshotId;
    std::string Secid;
    std::int32_t IntervalSec = 0;
    std::int64_t FromMs = 0;
    std::int64_t ToMs = 0;

    friend bool operator==(const BarCacheKey &, const BarCacheKey &) = default;
    friend auto operator<=>(const BarCacheKey &, const BarCacheKey &) = default;
  };

  struct BarCacheEntry {
    BarCacheKey Key;
    std::shared_ptr<const std::vector<core::Bar>> Bars;
  };

  void requireReadySnapshot(const std::string &SnapshotId);
  std::shared_ptr<const SnapshotSecurities>
  loadSnapshotSecurities(const std::string &SnapshotId);
  const core::Security &requireSecurity(const SnapshotSecurities &Securities,
                                        const std::string &Secid) const;
  std::shared_ptr<const StoredIntervalsMap>
  loadStoredIntervals(const std::string &SnapshotId);
  static bool hasStoredInterval(const StoredIntervalsMap &Stored,
                                const std::string &Secid,
                                std::int32_t IntervalSec);

  std::vector<core::Bar> fetchSingle(const core::BarRequest &Request,
                                     bool FromCandles);
  std::unordered_map<std::string, std::vector<core::Bar>>
  fetchBatch(const std::string &SnapshotId,
             const std::vector<std::string> &Secids, std::int32_t IntervalSec,
             std::int64_t FromMs, std::int64_t ToMs, bool FromCandles);

  std::shared_ptr<const std::vector<core::Bar>>
  getCachedBars(const BarCacheKey &Key);
  void putCachedBars(BarCacheKey Key,
                     std::shared_ptr<const std::vector<core::Bar>> Bars);
  void evictBarCacheWhileOverLimit();

  std::unique_ptr<ClickHouseClient> Client;

  mutable std::shared_mutex MetadataMutex;
  std::unordered_set<std::string> ReadySnapshots;
  std::unordered_map<std::string, std::shared_ptr<const SnapshotSecurities>>
      SecuritiesBySnapshot;
  std::unordered_map<std::string, std::shared_ptr<const StoredIntervalsMap>>
      StoredIntervalsBySnapshot;

  const std::size_t BarCacheMaxEntries;
  const std::size_t BarCacheMaxBars;
  mutable std::mutex BarCacheMutex;
  std::list<BarCacheEntry> BarCacheItems;
  std::map<BarCacheKey, std::list<BarCacheEntry>::iterator> BarCacheIndex;
  std::size_t BarCacheTotalBars = 0;
};

} // namespace worker
