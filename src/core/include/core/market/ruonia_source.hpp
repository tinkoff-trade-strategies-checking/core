#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/report/ruonia.hpp"

namespace core {

// Источник ряда RUONIA версии данных. Реализации детерминированы (INV-4).
class RuoniaSource {
public:
  virtual ~RuoniaSource() = default;

  // Возвращает ряд RUONIA с датами по возрастанию до UpToYmd включительно.
  // SnapshotNotFound, если версии данных нет или она не готова.
  virtual std::vector<DailyRate> ruoniaRates(const std::string &SnapshotId,
                                             std::int32_t UpToYmd) = 0;
};

// Ряд RUONIA в памяти: для тестов и CLI.
class InMemoryRuoniaSource final : public RuoniaSource {
public:
  // Добавляет или заменяет ряд RUONIA для версии данных. Ставки сортируются по
  // дате; дубликат даты - std::invalid_argument.
  void add(const std::string &SnapshotId, std::vector<DailyRate> Rates);

  std::vector<DailyRate> ruoniaRates(const std::string &SnapshotId,
                                     std::int32_t UpToYmd) override;

private:
  std::map<std::string, std::vector<DailyRate>> Rates;
};

} // namespace core
