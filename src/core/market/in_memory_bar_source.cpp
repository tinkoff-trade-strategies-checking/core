#include "core/market/in_memory_bar_source.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace core {

void InMemoryBarSource::addSecurity(const std::string &SnapshotId,
                                    const Security &Security) {
  Snapshots.insert(SnapshotId);
  Securities[{SnapshotId, Security.Secid}] = Security;
}

void InMemoryBarSource::addSeries(const std::string &SnapshotId,
                                  const std::string &Secid,
                                  std::int32_t IntervalSec,
                                  std::vector<Bar> Bars) {
  std::stable_sort(Bars.begin(), Bars.end(), [](const Bar &A, const Bar &B) {
    return A.TsOpenMs < B.TsOpenMs;
  });
  for (std::size_t I = 1; I < Bars.size(); ++I) {
    if (Bars[I].TsOpenMs == Bars[I - 1].TsOpenMs) {
      throw std::invalid_argument("InMemoryBarSource: дубликат tsOpenMs");
    }
  }
  Snapshots.insert(SnapshotId);
  Series[{SnapshotId, Secid, IntervalSec}] = std::move(Bars);
}

std::vector<Bar> InMemoryBarSource::load(const BarRequest &Request) {
  if (!Snapshots.contains(Request.SnapshotId)) {
    throw SnapshotNotFound("версия данных не найдена: " + Request.SnapshotId);
  }
  const auto It =
      Series.find({Request.SnapshotId, Request.Secid, Request.IntervalSec});
  if (It == Series.end()) {
    if (!Securities.contains({Request.SnapshotId, Request.Secid})) {
      throw SecurityNotFound("инструмент не найден: " + Request.Secid);
    }
    return {};
  }
  std::vector<Bar> Out;
  for (const Bar &Bar : It->second) {
    if (Bar.TsOpenMs < Request.FromMs || Bar.TsOpenMs >= Request.ToMs ||
        Bar.AvailableAtMs > Request.ToMs)
      continue;
    Out.push_back(Bar);
    Out.back().Index = static_cast<std::int64_t>(Out.size()) - 1;
  }
  return Out;
}

Security InMemoryBarSource::security(const std::string &SnapshotId,
                                     const std::string &Secid) {
  if (!Snapshots.contains(SnapshotId)) {
    throw SnapshotNotFound("версия данных не найдена: " + SnapshotId);
  }
  const auto It = Securities.find({SnapshotId, Secid});
  if (It == Securities.end())
    throw SecurityNotFound("инструмент не найден: " + Secid);
  return It->second;
}

} // namespace core
