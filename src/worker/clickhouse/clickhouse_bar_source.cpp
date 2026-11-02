#include "clickhouse/clickhouse_bar_source.hpp"

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include "core/money/decimal.hpp"

namespace worker {

namespace {

constexpr std::int32_t kMaxTradeAggregationSec = 86400;  // предел candles_from_trades

// UUID в каноническом виде 8-4-4-4-12; иное ClickHouse всё равно отвергнет, но так ответ
// однозначно SnapshotNotFound, а не ошибка сервера.
bool looksLikeUuid(const std::string& s) {
  if (s.size() != 36) return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

// Значение параметра в «экранированном» текстовом формате ClickHouse (строка с \t, \n, \\).
std::string escapeParam(const std::string& text) {
  std::string out;
  for (const char c : text) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::int64_t parseInt64(const std::string& text, const char* what) {
  std::int64_t value = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || ptr != text.data() + text.size()) {
    throw core::MarketDataError(std::string("ClickHouse: неверное число в поле ") + what + ": " + text);
  }
  return value;
}

core::Decimal parseDecimal(const std::string& text, const char* what) {
  try {
    return core::Decimal::parse(text);
  } catch (const std::exception&) {
    throw core::MarketDataError(std::string("ClickHouse: неверное десятичное в поле ") + what + ": " + text);
  }
}

void requireColumns(const std::vector<std::string>& row, std::size_t n) {
  if (row.size() != n) throw core::MarketDataError("ClickHouse: неожиданное число столбцов в ответе");
}

const char* const kCandlesSql =
    "SELECT toUnixTimestamp64Milli(ts_open), toUnixTimestamp64Milli(available_at), "
    "open, high, low, close, volume, value "
    "FROM candles "
    "WHERE snapshot_id = {snapshot_id:UUID} AND secid = {secid:String} AND interval_sec = {interval_sec:UInt32} "
    "AND ts_open >= fromUnixTimestamp64Milli({from_ms:Int64}, 'UTC') "
    "AND ts_open < fromUnixTimestamp64Milli({to_ms:Int64}, 'UTC') "
    "AND available_at <= fromUnixTimestamp64Milli({to_ms:Int64}, 'UTC') "
    "ORDER BY ts_open ASC FORMAT TabSeparated";

const char* const kTradesSql =
    "SELECT toUnixTimestamp64Milli(ts_open), toUnixTimestamp64Milli(available_at), "
    "open, high, low, close, volume, value "
    "FROM candles_from_trades(snapshot_id = {snapshot_id:UUID}, interval_sec = {interval_sec:UInt32}) "
    "WHERE secid = {secid:String} "
    "AND ts_open >= fromUnixTimestamp64Milli({from_ms:Int64}, 'UTC') "
    "AND ts_open < fromUnixTimestamp64Milli({to_ms:Int64}, 'UTC') "
    "AND available_at <= fromUnixTimestamp64Milli({to_ms:Int64}, 'UTC') "
    "ORDER BY ts_open ASC FORMAT TabSeparated";

}  // namespace

ClickHouseBarSource::ClickHouseBarSource(std::unique_ptr<ClickHouseClient> client)
    : client_(std::move(client)) {
  if (!client_) throw std::invalid_argument("ClickHouseBarSource: клиент не задан");
}

ClickHouseBarSource::ClickHouseBarSource(const ClickHouseConfig& config)
    : ClickHouseBarSource(std::make_unique<CurlClickHouseClient>(config)) {}

void ClickHouseBarSource::requireReadySnapshot(const std::string& snapshotId) {
  if (!looksLikeUuid(snapshotId)) throw core::SnapshotNotFound("версия данных не найдена: " + snapshotId);
  const auto rows = parseTabSeparated(client_->query(
      "SELECT count() FROM snapshots_ready WHERE snapshot_id = {snapshot_id:UUID} FORMAT TabSeparated",
      {{"snapshot_id", snapshotId}}));
  if (rows.size() != 1 || rows[0].size() != 1) throw core::MarketDataError("ClickHouse: неожиданный ответ snapshots_ready");
  if (parseInt64(rows[0][0], "count") == 0) {
    throw core::SnapshotNotFound("версия данных не найдена или не готова: " + snapshotId);
  }
}

core::Security ClickHouseBarSource::security(const std::string& snapshotId, const std::string& secid) {
  requireReadySnapshot(snapshotId);
  // Один secid на две записи (например, две площадки) - неоднозначно, выбирать «первую» нельзя (INV-4).
  if (requireSecurity(snapshotId, secid) > 1) {
    throw core::MarketDataError("в версии данных несколько записей инструмента: " + secid);
  }
  const auto rows = parseTabSeparated(client_->query(
      "SELECT secid, sector, lot_size, price_step FROM securities "
      "WHERE snapshot_id = {snapshot_id:UUID} AND secid = {secid:String} FORMAT TabSeparated",
      {{"snapshot_id", snapshotId}, {"secid", escapeParam(secid)}}));
  if (rows.empty()) throw core::SecurityNotFound("инструмент не найден: " + secid);
  requireColumns(rows[0], 4);
  core::Security sec;
  sec.secid = rows[0][0];
  sec.sector = rows[0][1];
  sec.lotSize = parseInt64(rows[0][2], "lot_size");
  sec.priceStep = parseDecimal(rows[0][3], "price_step");
  return sec;
}

std::int64_t ClickHouseBarSource::requireSecurity(const std::string& snapshotId, const std::string& secid) {
  const auto rows = parseTabSeparated(client_->query(
      "SELECT count() FROM securities WHERE snapshot_id = {snapshot_id:UUID} AND secid = {secid:String} "
      "FORMAT TabSeparated",
      {{"snapshot_id", snapshotId}, {"secid", escapeParam(secid)}}));
  if (rows.size() != 1 || rows[0].size() != 1) throw core::MarketDataError("ClickHouse: неожиданный ответ securities");
  const std::int64_t count = parseInt64(rows[0][0], "count");
  if (count == 0) throw core::SecurityNotFound("инструмент не найден: " + secid);
  return count;
}

std::vector<core::Bar> ClickHouseBarSource::load(const core::BarRequest& request) {
  if (request.intervalSec < 1) throw std::invalid_argument("BarRequest: intervalSec должен быть >= 1");
  requireReadySnapshot(request.snapshotId);
  requireSecurity(request.snapshotId, request.secid);
  if (request.fromMs >= request.toMs) return {};

  const QueryParams base{{"snapshot_id", request.snapshotId},
                         {"secid", escapeParam(request.secid)},
                         {"interval_sec", std::to_string(request.intervalSec)}};

  const auto stored = parseTabSeparated(client_->query(
      "SELECT count() FROM candles WHERE snapshot_id = {snapshot_id:UUID} AND secid = {secid:String} "
      "AND interval_sec = {interval_sec:UInt32} FORMAT TabSeparated",
      base));
  if (stored.size() != 1 || stored[0].size() != 1) throw core::MarketDataError("ClickHouse: неожиданный ответ candles");
  const bool fromCandles = parseInt64(stored[0][0], "count") > 0;

  if (!fromCandles && request.intervalSec > kMaxTradeAggregationSec) {
    throw IntervalUnavailable("интервал " + std::to_string(request.intervalSec) +
                                " с не хранится как свечи и не агрегируется из сделок (максимум 86400)");
  }

  QueryParams params = base;
  params.emplace_back("from_ms", std::to_string(request.fromMs));
  params.emplace_back("to_ms", std::to_string(request.toMs));
  const auto rows = parseTabSeparated(client_->query(fromCandles ? kCandlesSql : kTradesSql, params));

  std::vector<core::Bar> bars;
  bars.reserve(rows.size());
  for (const auto& row : rows) {
    requireColumns(row, 8);
    core::Bar bar;
    bar.index = static_cast<std::int64_t>(bars.size());
    bar.tsOpenMs = parseInt64(row[0], "ts_open");
    bar.availableAtMs = parseInt64(row[1], "available_at");
    bar.open = parseDecimal(row[2], "open");
    bar.high = parseDecimal(row[3], "high");
    bar.low = parseDecimal(row[4], "low");
    bar.close = parseDecimal(row[5], "close");
    bar.volume = parseInt64(row[6], "volume");
    bar.value = parseDecimal(row[7], "value");
    // INV-1: бар, который к toMs ещё не стал известен, не отдаётся (сервер уже отфильтровал; здесь защита).
    if (bar.availableAtMs > request.toMs || bar.availableAtMs < bar.tsOpenMs) {
      throw core::MarketDataError("ClickHouse: бар с недопустимым available_at (позже toMs или раньше ts_open)");
    }
    if (!bars.empty() && bar.tsOpenMs <= bars.back().tsOpenMs) {
      throw core::MarketDataError("ClickHouse: бары не упорядочены по ts_open (дубликат в данных?)");
    }
    bars.push_back(bar);
  }
  return bars;
}

}  // namespace worker
