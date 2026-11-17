#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/market/bar_source.hpp"

namespace core {

// Источник в памяти: для тестов и CLI. Результат не зависит от порядка
// добавления.
class InMemoryBarSource final : public BarSource {
public:
  // Регистрирует инструмент в версии данных (и саму версию).
  void addSecurity(const std::string &SnapshotId, const Security &Security);
  // Добавляет серию. Бары сортируются по tsOpenMs; index из аргумента
  // игнорируется (при load нумеруется заново). Дубликаты tsOpenMs -
  // std::invalid_argument. Заменяет серию целиком.
  void addSeries(const std::string &SnapshotId, const std::string &Secid,
                 std::int32_t IntervalSec, std::vector<Bar> Bars);

  std::vector<Bar> load(const BarRequest &Request) override;
  Security security(const std::string &SnapshotId,
                    const std::string &Secid) override;

private:
  using SeriesKey = std::tuple<std::string, std::string, std::int32_t>;
  std::set<std::string> Snapshots;
  std::map<std::pair<std::string, std::string>, Security> Securities;
  std::map<SeriesKey, std::vector<Bar>> Series;
};

} // namespace core
