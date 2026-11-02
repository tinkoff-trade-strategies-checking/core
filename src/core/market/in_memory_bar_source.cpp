#include "core/market/in_memory_bar_source.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace core {

void InMemoryBarSource::addSecurity(const std::string& snapshotId, const Security& security) {
  snapshots_.insert(snapshotId);
  securities_[{snapshotId, security.secid}] = security;
}

void InMemoryBarSource::addSeries(const std::string& snapshotId, const std::string& secid,
                                  std::int32_t intervalSec, std::vector<Bar> bars) {
  std::stable_sort(bars.begin(), bars.end(),
                   [](const Bar& a, const Bar& b) { return a.tsOpenMs < b.tsOpenMs; });
  for (std::size_t i = 1; i < bars.size(); ++i) {
    if (bars[i].tsOpenMs == bars[i - 1].tsOpenMs) {
      throw std::invalid_argument("InMemoryBarSource: дубликат tsOpenMs");
    }
  }
  snapshots_.insert(snapshotId);
  series_[{snapshotId, secid, intervalSec}] = std::move(bars);
}

std::vector<Bar> InMemoryBarSource::load(const BarRequest& request) {
  if (!snapshots_.contains(request.snapshotId)) {
    throw SnapshotNotFound("версия данных не найдена: " + request.snapshotId);
  }
  const auto it = series_.find({request.snapshotId, request.secid, request.intervalSec});
  if (it == series_.end()) {
    if (!securities_.contains({request.snapshotId, request.secid})) {
      throw SecurityNotFound("инструмент не найден: " + request.secid);
    }
    return {};
  }
  std::vector<Bar> out;
  for (const Bar& bar : it->second) {
    if (bar.tsOpenMs < request.fromMs || bar.tsOpenMs >= request.toMs || bar.availableAtMs > request.toMs) continue;
    out.push_back(bar);
    out.back().index = static_cast<std::int64_t>(out.size()) - 1;
  }
  return out;
}

Security InMemoryBarSource::security(const std::string& snapshotId, const std::string& secid) {
  if (!snapshots_.contains(snapshotId)) {
    throw SnapshotNotFound("версия данных не найдена: " + snapshotId);
  }
  const auto it = securities_.find({snapshotId, secid});
  if (it == securities_.end()) throw SecurityNotFound("инструмент не найден: " + secid);
  return it->second;
}

}  // namespace core
