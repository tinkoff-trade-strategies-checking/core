#pragma once

#include <memory>
#include <string>
#include <vector>

#include "clickhouse/clickhouse_client.hpp"
#include "core/market/bar_source.hpp"

namespace worker {

// Чтение баров из ClickHouse (ФТ-3, ФТ-5); таблицы и представления описаны в
// backend/deploy/clickhouse/schema. Правила:
//  - читаются только версии данных из `snapshots_ready` (иначе
//  SnapshotNotFound), INV-5;
//  - если в версии для (secid, interval) есть свечи в `candles` - берутся они;
//  иначе свечи
//    агрегируются из сделок представлением `candles_from_trades`
//    (интервал 1...86400 с); выбор делается по наличию, а не по диапазону: один
//    запрос не смешивает два источника;
//  - INV-1: в результат попадают только бары, уже известные к toMs
//  (available_at <= toMs);
//  - порядок строго по ts_open, поэтому результат детерминирован (INV-4);
//  - цены разбираются из текста через Decimal::parse, double не используется
//  (INV-6);
//  - availableAtMs - `available_at` из данных (для свечей из сделок - закрытие
//  свечи, не позже
//    конца бара; обрезка по концу сессии не делается, что только осторожнее для
//    INV-1);
//  - серия читается целиком одним запросом; потоковое чтение для очень больших
//  диапазонов - позже.
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
