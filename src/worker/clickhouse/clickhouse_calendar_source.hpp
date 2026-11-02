#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "clickhouse/clickhouse_client.hpp"
#include "core/market/calendar_source.hpp"

namespace worker {

// Торговый календарь версии данных из таблицы `trading_calendar` ClickHouse (ФТ-3, ФТ-5).
//  - читаются только версии из `snapshots_ready`, иначе SnapshotNotFound (INV-5);
//  - даты - дни по Москве (yyyymmdd), как хранит импортёр; порядок строго по дате (INV-4);
//  - дубликат даты в данных - MarketDataError, молча выбирать одну из строк нельзя;
//  - календарь строит импортёр по акциям, а не по индексу IMOEX (его свечи есть в версии
//    данных, но индекс торгуется не во все дни работы биржи) - адаптер ничего не выводит сам.
class ClickHouseCalendarSource final : public core::CalendarSource {
 public:
  explicit ClickHouseCalendarSource(std::unique_ptr<ClickHouseClient> client);
  // Боевая сборка: libcurl-клиент по настройкам.
  explicit ClickHouseCalendarSource(const ClickHouseConfig& config);

  // Дни с fromYmd по toYmd включительно. fromYmd > toYmd - пустой список; неверная дата
  // (не существует в григорианском календаре) - std::invalid_argument.
  std::vector<core::TradingDay> tradingDays(const std::string& snapshotId, std::int32_t fromYmd,
                                            std::int32_t toYmd) override;

 private:
  std::unique_ptr<ClickHouseClient> client_;
};

}  // namespace worker
