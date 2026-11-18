#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "clickhouse/clickhouse_client.hpp"
#include "core/market/ruonia_source.hpp"

namespace worker {

// Дневной ряд ставок RUONIA из таблицы `ruonia` ClickHouse (ФТ-17,
// documentation#40, A7).
//  - читаются только версии из `snapshots_ready`, иначе SnapshotNotFound
//  (INV-5);
//  - даты - дни по Москве (yyyymmdd); порядок строго по дате (INV-4);
//  - дубликат даты в данных - MarketDataError.
class ClickHouseRuoniaSource final : public core::RuoniaSource {
public:
  explicit ClickHouseRuoniaSource(std::unique_ptr<ClickHouseClient> Client);
  explicit ClickHouseRuoniaSource(const ClickHouseConfig &Config);

  std::vector<core::DailyRate> ruoniaRates(const std::string &SnapshotId,
                                           std::int32_t UpToYmd) override;

private:
  std::unique_ptr<ClickHouseClient> Client;
};

} // namespace worker
