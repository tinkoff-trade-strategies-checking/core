#pragma once

#include <memory>
#include <string>
#include <vector>

#include "clickhouse/clickhouse_client.hpp"
#include "core/market/bar_source.hpp"

namespace worker {

// Чтение баров из ClickHouse (ФТ-3, ФТ-5); таблицы описаны в
// core/AGENTS.md. Правила:
//  - читаются только версии данных из `snapshots_ready` (иначе
//  SnapshotNotFound), INV-5;
//  - если в версии для (secid, interval) есть свечи в `candles` — берутся они;
//  иначе свечи
//    агрегируются из минутных свечей (`interval_sec = 60`) средствами ClickHouse
//    (интервал 60...86400 с, кратный 60 с); выбор делается по наличию, а не по диапазону: один
//    запрос не смешивает два источника;
//  - INV-1: в результат попадают только бары, уже известные к toMs
//  (available_at <= toMs);
//  - порядок строго по ts_open, поэтому результат детерминирован (INV-4);
//  - цены разбираются из текста через Decimal::parse, double не используется
//  (INV-6);
//  - availableAtMs — `available_at` из данных (для агрегированных свечей —
//  `available_at` последней минутной свечи интервала, INV-1);
//  - серия читается целиком одним запросом; потоковое чтение для очень больших
//  диапазонов — позже.
class ClickHouseBarSource final : public core::BarSource {
public:
  explicit ClickHouseBarSource(std::unique_ptr<ClickHouseClient> Client);
  // Боевая сборка: libcurl-клиент по настройкам.
  explicit ClickHouseBarSource(const ClickHouseConfig &Config);

  std::vector<core::Bar> load(const core::BarRequest &Request) override;
  core::Security security(const std::string &SnapshotId,
                          const std::string &Secid) override;

private:
  void requireReadySnapshot(const std::string &SnapshotId);
  // Возвращает число записей инструмента в версии (>= 1); SecurityNotFound,
  // если нет.
  std::int64_t requireSecurity(const std::string &SnapshotId,
                               const std::string &Secid);

  std::unique_ptr<ClickHouseClient> Client;
};

} // namespace worker
