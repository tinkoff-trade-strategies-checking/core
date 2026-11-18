#include "core/market/ruonia_source.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "core/market/bar_source.hpp" // SnapshotNotFound

namespace core {

void InMemoryRuoniaSource::add(const std::string &SnapshotId,
                               std::vector<DailyRate> SnapshotRates) {
  std::stable_sort(SnapshotRates.begin(), SnapshotRates.end(),
                   [](const DailyRate &A, const DailyRate &B) {
                     return A.Yyyymmdd < B.Yyyymmdd;
                   });
  for (std::size_t I = 1; I < SnapshotRates.size(); ++I) {
    if (SnapshotRates[I].Yyyymmdd == SnapshotRates[I - 1].Yyyymmdd) {
      throw std::invalid_argument("InMemoryRuoniaSource: дубликат даты: " +
                                  std::to_string(SnapshotRates[I].Yyyymmdd));
    }
  }
  Rates[SnapshotId] = std::move(SnapshotRates);
}

std::vector<DailyRate>
InMemoryRuoniaSource::ruoniaRates(const std::string &SnapshotId,
                                  std::int32_t UpToYmd) {
  const auto It = Rates.find(SnapshotId);
  if (It == Rates.end())
    throw SnapshotNotFound("ряд RUONIA версии данных не найден: " + SnapshotId);
  std::vector<DailyRate> Out;
  for (const DailyRate &R : It->second) {
    if (R.Yyyymmdd <= UpToYmd)
      Out.push_back(R);
  }
  return Out;
}

} // namespace core
