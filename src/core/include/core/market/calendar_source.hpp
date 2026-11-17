#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace core {

// День календаря: дата в московском времени (yyyymmdd) и признак торгового дня.
// Торговый день - день, когда были свечи хотя бы одной бумаги (выходные тоже
// бывают торговыми).
struct TradingDay {
  std::int32_t yyyymmdd = 0;
  bool isTrading = false;

  friend bool operator==(const TradingDay &, const TradingDay &) = default;
};

// Источник торгового календаря версии данных. Реализации детерминированы
// (INV-4).
class CalendarSource {
public:
  virtual ~CalendarSource() = default;

  // Дни календаря с fromYmd по toYmd включительно, по возрастанию даты.
  // SnapshotNotFound, если версии данных нет.
  virtual std::vector<TradingDay> tradingDays(const std::string &snapshotId,
                                              std::int32_t fromYmd,
                                              std::int32_t toYmd) = 0;
};

// Календарь в памяти: для тестов и CLI.
class InMemoryCalendarSource final : public CalendarSource {
public:
  // Заменяет календарь версии данных целиком. Дни сортируются по дате; дубликат
  // даты - std::invalid_argument.
  void add(const std::string &snapshotId, std::vector<TradingDay> days);

  std::vector<TradingDay> tradingDays(const std::string &snapshotId,
                                      std::int32_t fromYmd,
                                      std::int32_t toYmd) override;

private:
  std::map<std::string, std::vector<TradingDay>> days_;
};

} // namespace core
