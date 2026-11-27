#include "clickhouse/clickhouse_bar_source.hpp"

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include "core/money/decimal.hpp"

namespace worker {

namespace {

// UUID в каноническом виде 8-4-4-4-12; иное ClickHouse всё равно отвергнет, но
// так ответ однозначно SnapshotNotFound, а не ошибка сервера.
bool looksLikeUuid(const std::string &S) {
  if (S.size() != 36)
    return false;
  for (std::size_t I = 0; I < S.size(); ++I) {
    const char C = S[I];
    if (I == 8 || I == 13 || I == 18 || I == 23) {
      if (C != '-')
        return false;
    } else if (!((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f') ||
                 (C >= 'A' && C <= 'F'))) {
      return false;
    }
  }
  return true;
}

// Значение параметра в «экранированном» текстовом формате ClickHouse (строка с
// \t, \n, \\).
std::string escapeParam(const std::string &Text) {
  std::string Out;
  for (const char C : Text) {
    switch (C) {
    case '\\':
      Out += "\\\\";
      break;
    case '\t':
      Out += "\\t";
      break;
    case '\n':
      Out += "\\n";
      break;
    case '\r':
      Out += "\\r";
      break;
    default:
      Out.push_back(C);
      break;
    }
  }
  return Out;
}

// Форматирует вектор строк в текстовый литерал ClickHouse Array(String)
// (['a','b']) для безопасной передачи в параметре {secids:Array(String)}.
std::string formatStringArrayParam(const std::vector<std::string> &Items) {
  std::string Out = "[";
  for (std::size_t I = 0; I < Items.size(); ++I) {
    if (I > 0)
      Out.push_back(',');
    Out.push_back('\'');
    for (const char C : Items[I]) {
      switch (C) {
      case '\\':
        Out += "\\\\";
        break;
      case '\'':
        Out += "\\'";
        break;
      case '\t':
        Out += "\\t";
        break;
      case '\n':
        Out += "\\n";
        break;
      case '\r':
        Out += "\\r";
        break;
      case '\0':
        Out += "\\0";
        break;
      default:
        Out.push_back(C);
        break;
      }
    }
    Out.push_back('\'');
  }
  Out.push_back(']');
  return Out;
}

std::int64_t parseInt64(const std::string &Text, const char *What) {
  std::int64_t Value = 0;
  const auto [Ptr, Ec] =
      std::from_chars(Text.data(), Text.data() + Text.size(), Value);
  if (Ec != std::errc() || Ptr != Text.data() + Text.size()) {
    throw core::MarketDataError(
        std::string("ClickHouse: неверное число в поле ") + What + ": " + Text);
  }
  return Value;
}

core::Decimal parseDecimal(const std::string &Text, const char *What) {
  try {
    return core::Decimal::parse(Text);
  } catch (const std::exception &) {
    throw core::MarketDataError(
        std::string("ClickHouse: неверное десятичное в поле ") + What + ": " +
        Text);
  }
}

void requireColumns(const std::vector<std::string> &Row, std::size_t N) {
  if (Row.size() != N)
    throw core::MarketDataError(
        "ClickHouse: неожиданное число столбцов в ответе");
}

void validateIntervalSec(std::int32_t IntervalSec) {
  if (IntervalSec < 1) {
    throw std::invalid_argument("BarRequest: intervalSec должен быть >= 1");
  }
  if (IntervalSec < 60) {
    throw IntervalUnavailable("интервал " + std::to_string(IntervalSec) +
                              " с меньше 1 мин и не хранится в архиве");
  }
  if (IntervalSec > 86400) {
    throw IntervalUnavailable("интервал " + std::to_string(IntervalSec) +
                              " с не хранится как свечи и не агрегируется из "
                              "минутных свечей (максимум 86400)");
  }
  if (IntervalSec % 60 != 0) {
    throw IntervalUnavailable("интервал " + std::to_string(IntervalSec) +
                              " с не кратен 1 мин");
  }
}

void appendParsedBar(const std::vector<std::string> &Row, std::size_t Offset,
                     std::int64_t ToMs, std::vector<core::Bar> &Bars) {
  core::Bar Bar;
  Bar.Index = static_cast<std::int64_t>(Bars.size());
  Bar.TsOpenMs = parseInt64(Row[Offset + 0], "ts_open");
  Bar.AvailableAtMs = parseInt64(Row[Offset + 1], "available_at");
  Bar.Open = parseDecimal(Row[Offset + 2], "open");
  Bar.High = parseDecimal(Row[Offset + 3], "high");
  Bar.Low = parseDecimal(Row[Offset + 4], "low");
  Bar.Close = parseDecimal(Row[Offset + 5], "close");
  Bar.Volume = parseInt64(Row[Offset + 6], "volume");
  Bar.Value = parseDecimal(Row[Offset + 7], "value");
  // INV-1: бар, который к toMs ещё не стал известен, не отдаётся (сервер уже
  // отфильтровал; здесь защита).
  if (Bar.AvailableAtMs > ToMs || Bar.AvailableAtMs < Bar.TsOpenMs) {
    throw core::MarketDataError("ClickHouse: бар с недопустимым available_at "
                                "(позже toMs или раньше ts_open)");
  }
  if (!Bars.empty() && Bar.TsOpenMs <= Bars.back().TsOpenMs) {
    throw core::MarketDataError(
        "ClickHouse: бары не упорядочены по ts_open (дубликат в данных?)");
  }
  Bars.push_back(Bar);
}

const char *const kCandlesSql =
    "SELECT toUnixTimestamp64Milli(ts_open), "
    "toUnixTimestamp64Milli(available_at), "
    "open, high, low, close, volume, value "
    "FROM candles "
    "WHERE snapshot_id = {snapshot_id:UUID} AND secid = {secid:String} AND "
    "interval_sec = {interval_sec:UInt32} "
    "AND ts_open >= fromUnixTimestamp64Milli({from_ms:Int64}, 'UTC') "
    "AND ts_open < fromUnixTimestamp64Milli({to_ms:Int64}, 'UTC') "
    "AND available_at <= fromUnixTimestamp64Milli({to_ms:Int64}, 'UTC') "
    "ORDER BY ts_open ASC FORMAT TabSeparated";

// Агрегированный бар [T, T+Δ) доступен в T+Δ (INV-1, решение владельца
// продукта 18.11.2026), а не по последней минутке; для 1 д — конец суток МСК
// (bar_ts_open + 86400 с: сезонные переходы времени в РФ до 2014 года
// приходились на воскресенья, когда торгов нет, поэтому в торговые дни сутки
// МСК всегда равны 86400 с).
const char *const kAggregateMinutesSql =
    "SELECT intDiv(toUnixTimestamp64Milli(ts_open), {interval_ms:Int64}) * "
    "{interval_ms:Int64} AS bar_ts_open, "
    "bar_ts_open + {interval_ms:Int64} AS bar_available_at, "
    "argMin(open, ts_open) AS bar_open, "
    "max(high) AS bar_high, "
    "min(low) AS bar_low, "
    "argMax(close, ts_open) AS bar_close, "
    "sum(volume) AS bar_volume, "
    "sum(value) AS bar_value "
    "FROM candles "
    "WHERE snapshot_id = {snapshot_id:UUID} AND secid = {secid:String} AND "
    "interval_sec = 60 "
    "AND ts_open >= fromUnixTimestamp64Milli({from_ms:Int64}, 'UTC') "
    "AND ts_open < fromUnixTimestamp64Milli({to_ms:Int64} + "
    "{interval_ms:Int64}, 'UTC') "
    "GROUP BY bar_ts_open "
    "HAVING bar_ts_open >= {from_ms:Int64} "
    "AND bar_ts_open < {to_ms:Int64} "
    "AND bar_available_at <= {to_ms:Int64} "
    "ORDER BY bar_ts_open ASC FORMAT TabSeparated";

const char *const kAggregateDailySql =
    "SELECT toUnixTimestamp64Milli(toDateTime64(toStartOfDay(ts_open, "
    "'Europe/Moscow'), 3, 'Europe/Moscow')) AS bar_ts_open, "
    "bar_ts_open + {interval_ms:Int64} AS bar_available_at, "
    "argMin(open, ts_open) AS bar_open, "
    "max(high) AS bar_high, "
    "min(low) AS bar_low, "
    "argMax(close, ts_open) AS bar_close, "
    "sum(volume) AS bar_volume, "
    "sum(value) AS bar_value "
    "FROM candles "
    "WHERE snapshot_id = {snapshot_id:UUID} AND secid = {secid:String} AND "
    "interval_sec = 60 "
    "AND ts_open >= fromUnixTimestamp64Milli({from_ms:Int64}, 'UTC') "
    "AND ts_open < fromUnixTimestamp64Milli({to_ms:Int64} + "
    "{interval_ms:Int64}, 'UTC') "
    "GROUP BY bar_ts_open "
    "HAVING bar_ts_open >= {from_ms:Int64} "
    "AND bar_ts_open < {to_ms:Int64} "
    "AND bar_available_at <= {to_ms:Int64} "
    "ORDER BY bar_ts_open ASC FORMAT TabSeparated";

const char *const kBatchCandlesSql =
    "SELECT secid, toUnixTimestamp64Milli(ts_open), "
    "toUnixTimestamp64Milli(available_at), "
    "open, high, low, close, volume, value "
    "FROM candles "
    "WHERE snapshot_id = {snapshot_id:UUID} AND secid IN "
    "{secids:Array(String)} AND interval_sec = {interval_sec:UInt32} "
    "AND ts_open >= fromUnixTimestamp64Milli({from_ms:Int64}, 'UTC') "
    "AND ts_open < fromUnixTimestamp64Milli({to_ms:Int64}, 'UTC') "
    "AND available_at <= fromUnixTimestamp64Milli({to_ms:Int64}, 'UTC') "
    "ORDER BY secid ASC, ts_open ASC FORMAT TabSeparated";

const char *const kBatchAggregateMinutesSql =
    "SELECT secid, "
    "intDiv(toUnixTimestamp64Milli(ts_open), {interval_ms:Int64}) * "
    "{interval_ms:Int64} AS bar_ts_open, "
    "bar_ts_open + {interval_ms:Int64} AS bar_available_at, "
    "argMin(open, ts_open) AS bar_open, "
    "max(high) AS bar_high, "
    "min(low) AS bar_low, "
    "argMax(close, ts_open) AS bar_close, "
    "sum(volume) AS bar_volume, "
    "sum(value) AS bar_value "
    "FROM candles "
    "WHERE snapshot_id = {snapshot_id:UUID} AND secid IN "
    "{secids:Array(String)} AND interval_sec = 60 "
    "AND ts_open >= fromUnixTimestamp64Milli({from_ms:Int64}, 'UTC') "
    "AND ts_open < fromUnixTimestamp64Milli({to_ms:Int64} + "
    "{interval_ms:Int64}, 'UTC') "
    "GROUP BY secid, bar_ts_open "
    "HAVING bar_ts_open >= {from_ms:Int64} "
    "AND bar_ts_open < {to_ms:Int64} "
    "AND bar_available_at <= {to_ms:Int64} "
    "ORDER BY secid ASC, bar_ts_open ASC FORMAT TabSeparated";

const char *const kBatchAggregateDailySql =
    "SELECT secid, "
    "toUnixTimestamp64Milli(toDateTime64(toStartOfDay(ts_open, "
    "'Europe/Moscow'), 3, 'Europe/Moscow')) AS bar_ts_open, "
    "bar_ts_open + {interval_ms:Int64} AS bar_available_at, "
    "argMin(open, ts_open) AS bar_open, "
    "max(high) AS bar_high, "
    "min(low) AS bar_low, "
    "argMax(close, ts_open) AS bar_close, "
    "sum(volume) AS bar_volume, "
    "sum(value) AS bar_value "
    "FROM candles "
    "WHERE snapshot_id = {snapshot_id:UUID} AND secid IN "
    "{secids:Array(String)} AND interval_sec = 60 "
    "AND ts_open >= fromUnixTimestamp64Milli({from_ms:Int64}, 'UTC') "
    "AND ts_open < fromUnixTimestamp64Milli({to_ms:Int64} + "
    "{interval_ms:Int64}, 'UTC') "
    "GROUP BY secid, bar_ts_open "
    "HAVING bar_ts_open >= {from_ms:Int64} "
    "AND bar_ts_open < {to_ms:Int64} "
    "AND bar_available_at <= {to_ms:Int64} "
    "ORDER BY secid ASC, bar_ts_open ASC FORMAT TabSeparated";

} // namespace

ClickHouseBarSource::ClickHouseBarSource(
    std::unique_ptr<ClickHouseClient> HttpClient, std::size_t MaxCacheEntries,
    std::size_t MaxCacheBars)
    : Client(std::move(HttpClient)), BarCacheMaxEntries(MaxCacheEntries),
      BarCacheMaxBars(MaxCacheBars) {
  if (!Client)
    throw std::invalid_argument("ClickHouseBarSource: клиент не задан");
}

ClickHouseBarSource::ClickHouseBarSource(const ClickHouseConfig &Config,
                                         std::size_t MaxCacheEntries,
                                         std::size_t MaxCacheBars)
    : ClickHouseBarSource(std::make_unique<CurlClickHouseClient>(Config),
                          MaxCacheEntries, MaxCacheBars) {}

void ClickHouseBarSource::requireReadySnapshot(const std::string &SnapshotId) {
  if (!looksLikeUuid(SnapshotId))
    throw core::SnapshotNotFound("версия данных не найдена: " + SnapshotId);
  {
    std::shared_lock<std::shared_mutex> Lock(MetadataMutex);
    if (ReadySnapshots.contains(SnapshotId))
      return;
  }
  const auto Rows = parseTabSeparated(
      Client->query("SELECT count() FROM snapshots_ready WHERE snapshot_id = "
                    "{snapshot_id:UUID} FORMAT TabSeparated",
                    {{"snapshot_id", SnapshotId}}));
  if (Rows.size() != 1 || Rows[0].size() != 1)
    throw core::MarketDataError(
        "ClickHouse: неожиданный ответ snapshots_ready");
  if (parseInt64(Rows[0][0], "count") == 0) {
    throw core::SnapshotNotFound("версия данных не найдена или не готова: " +
                                 SnapshotId);
  }
  std::unique_lock<std::shared_mutex> Lock(MetadataMutex);
  ReadySnapshots.insert(SnapshotId);
}

std::shared_ptr<const ClickHouseBarSource::SnapshotSecurities>
ClickHouseBarSource::loadSnapshotSecurities(const std::string &SnapshotId) {
  {
    std::shared_lock<std::shared_mutex> Lock(MetadataMutex);
    const auto It = SecuritiesBySnapshot.find(SnapshotId);
    if (It != SecuritiesBySnapshot.end())
      return It->second;
  }
  const auto Rows = parseTabSeparated(
      Client->query("SELECT secid, sector, lot_size, price_step FROM "
                    "securities WHERE snapshot_id = {snapshot_id:UUID} "
                    "FORMAT TabSeparated",
                    {{"snapshot_id", SnapshotId}}));
  auto Loaded = std::make_shared<SnapshotSecurities>();
  for (const auto &Row : Rows) {
    requireColumns(Row, 4);
    const std::string &Secid = Row[0];
    if (Loaded->AmbiguousSecids.contains(Secid))
      continue;
    if (Loaded->BySecid.contains(Secid)) {
      Loaded->BySecid.erase(Secid);
      Loaded->AmbiguousSecids.insert(Secid);
      continue;
    }
    core::Security Sec;
    Sec.Secid = Secid;
    Sec.Sector = Row[1];
    Sec.LotSize = parseInt64(Row[2], "lot_size");
    Sec.PriceStep = parseDecimal(Row[3], "price_step");
    Loaded->BySecid.emplace(Secid, std::move(Sec));
  }
  std::unique_lock<std::shared_mutex> Lock(MetadataMutex);
  const auto [It, Inserted] =
      SecuritiesBySnapshot.emplace(SnapshotId, std::move(Loaded));
  return It->second;
}

const core::Security &
ClickHouseBarSource::requireSecurity(const SnapshotSecurities &Securities,
                                     const std::string &Secid) const {
  // Один secid на две записи (например, две площадки) - неоднозначно, выбирать
  // «первую» нельзя (INV-4).
  if (Securities.AmbiguousSecids.contains(Secid)) {
    throw core::MarketDataError(
        "в версии данных несколько записей инструмента: " + Secid);
  }
  const auto It = Securities.BySecid.find(Secid);
  if (It == Securities.BySecid.end())
    throw core::SecurityNotFound("инструмент не найден: " + Secid);
  return It->second;
}

std::shared_ptr<const ClickHouseBarSource::StoredIntervalsMap>
ClickHouseBarSource::loadStoredIntervals(const std::string &SnapshotId) {
  {
    std::shared_lock<std::shared_mutex> Lock(MetadataMutex);
    const auto It = StoredIntervalsBySnapshot.find(SnapshotId);
    if (It != StoredIntervalsBySnapshot.end())
      return It->second;
  }
  const auto Rows = parseTabSeparated(
      Client->query("SELECT DISTINCT secid, interval_sec FROM candles WHERE "
                    "snapshot_id = {snapshot_id:UUID} FORMAT TabSeparated",
                    {{"snapshot_id", SnapshotId}}));
  auto Loaded = std::make_shared<StoredIntervalsMap>();
  for (const auto &Row : Rows) {
    requireColumns(Row, 2);
    const std::int64_t Interval = parseInt64(Row[1], "interval_sec");
    (*Loaded)[Row[0]].insert(static_cast<std::int32_t>(Interval));
  }
  std::unique_lock<std::shared_mutex> Lock(MetadataMutex);
  const auto [It, Inserted] =
      StoredIntervalsBySnapshot.emplace(SnapshotId, std::move(Loaded));
  return It->second;
}

bool ClickHouseBarSource::hasStoredInterval(const StoredIntervalsMap &Stored,
                                            const std::string &Secid,
                                            std::int32_t IntervalSec) {
  const auto It = Stored.find(Secid);
  return It != Stored.end() && It->second.contains(IntervalSec);
}

core::Security ClickHouseBarSource::security(const std::string &SnapshotId,
                                             const std::string &Secid) {
  requireReadySnapshot(SnapshotId);
  const auto Securities = loadSnapshotSecurities(SnapshotId);
  return requireSecurity(*Securities, Secid);
}

std::shared_ptr<const std::vector<core::Bar>>
ClickHouseBarSource::getCachedBars(const BarCacheKey &Key) {
  if (BarCacheMaxEntries == 0 || BarCacheMaxBars == 0)
    return nullptr;
  std::lock_guard<std::mutex> Lock(BarCacheMutex);
  const auto It = BarCacheIndex.find(Key);
  if (It == BarCacheIndex.end())
    return nullptr;
  BarCacheItems.splice(BarCacheItems.begin(), BarCacheItems, It->second);
  return It->second->Bars;
}

void ClickHouseBarSource::evictBarCacheWhileOverLimit() {
  while (!BarCacheItems.empty() && (BarCacheItems.size() > BarCacheMaxEntries ||
                                    BarCacheTotalBars > BarCacheMaxBars)) {
    const auto Last = --BarCacheItems.end();
    BarCacheTotalBars -= Last->Bars->size();
    BarCacheIndex.erase(Last->Key);
    BarCacheItems.pop_back();
  }
}

void ClickHouseBarSource::putCachedBars(
    BarCacheKey Key, std::shared_ptr<const std::vector<core::Bar>> Bars) {
  if (!Bars || BarCacheMaxEntries == 0 || BarCacheMaxBars == 0)
    return;
  const std::size_t Count = Bars->size();
  std::lock_guard<std::mutex> Lock(BarCacheMutex);
  const auto It = BarCacheIndex.find(Key);
  if (Count > BarCacheMaxBars) {
    if (It != BarCacheIndex.end()) {
      BarCacheTotalBars -= It->second->Bars->size();
      BarCacheItems.erase(It->second);
      BarCacheIndex.erase(It);
    }
    return;
  }
  if (It != BarCacheIndex.end()) {
    BarCacheTotalBars -= It->second->Bars->size();
    It->second->Bars = std::move(Bars);
    BarCacheTotalBars += Count;
    BarCacheItems.splice(BarCacheItems.begin(), BarCacheItems, It->second);
    evictBarCacheWhileOverLimit();
    return;
  }

  BarCacheItems.push_front(BarCacheEntry{Key, std::move(Bars)});
  try {
    BarCacheIndex.emplace(std::move(Key), BarCacheItems.begin());
  } catch (...) {
    BarCacheItems.pop_front();
    throw;
  }
  BarCacheTotalBars += Count;
  evictBarCacheWhileOverLimit();
}

std::vector<core::Bar>
ClickHouseBarSource::fetchSingle(const core::BarRequest &Request,
                                 bool FromCandles) {
  std::vector<std::vector<std::string>> Rows;
  if (FromCandles) {
    QueryParams Params{{"snapshot_id", Request.SnapshotId},
                       {"secid", escapeParam(Request.Secid)},
                       {"interval_sec", std::to_string(Request.IntervalSec)},
                       {"from_ms", std::to_string(Request.FromMs)},
                       {"to_ms", std::to_string(Request.ToMs)}};
    Rows = parseTabSeparated(Client->query(kCandlesSql, Params));
  } else {
    QueryParams Params{
        {"snapshot_id", Request.SnapshotId},
        {"secid", escapeParam(Request.Secid)},
        {"interval_ms",
         std::to_string(static_cast<std::int64_t>(Request.IntervalSec) * 1000)},
        {"from_ms", std::to_string(Request.FromMs)},
        {"to_ms", std::to_string(Request.ToMs)}};
    const char *const Sql = Request.IntervalSec == 86400 ? kAggregateDailySql
                                                         : kAggregateMinutesSql;
    Rows = parseTabSeparated(Client->query(Sql, Params));
  }

  std::vector<core::Bar> Bars;
  Bars.reserve(Rows.size());
  for (const auto &Row : Rows) {
    requireColumns(Row, 8);
    appendParsedBar(Row, 0, Request.ToMs, Bars);
  }
  return Bars;
}

std::unordered_map<std::string, std::vector<core::Bar>>
ClickHouseBarSource::fetchBatch(const std::string &SnapshotId,
                                const std::vector<std::string> &Secids,
                                std::int32_t IntervalSec, std::int64_t FromMs,
                                std::int64_t ToMs, bool FromCandles) {
  std::unordered_map<std::string, std::vector<core::Bar>> Result;
  Result.reserve(Secids.size());
  for (const std::string &Secid : Secids) {
    Result.emplace(Secid, std::vector<core::Bar>{});
  }
  if (Secids.empty())
    return Result;

  if (Secids.size() == 1) {
    core::BarRequest Single{SnapshotId, Secids.front(), IntervalSec, FromMs,
                            ToMs};
    Result[Secids.front()] = fetchSingle(Single, FromCandles);
    return Result;
  }

  std::vector<std::vector<std::string>> Rows;
  if (FromCandles) {
    QueryParams Params{{"snapshot_id", SnapshotId},
                       {"secids", formatStringArrayParam(Secids)},
                       {"interval_sec", std::to_string(IntervalSec)},
                       {"from_ms", std::to_string(FromMs)},
                       {"to_ms", std::to_string(ToMs)}};
    Rows = parseTabSeparated(Client->query(kBatchCandlesSql, Params));
  } else {
    QueryParams Params{
        {"snapshot_id", SnapshotId},
        {"secids", formatStringArrayParam(Secids)},
        {"interval_ms",
         std::to_string(static_cast<std::int64_t>(IntervalSec) * 1000)},
        {"from_ms", std::to_string(FromMs)},
        {"to_ms", std::to_string(ToMs)}};
    const char *const Sql = IntervalSec == 86400 ? kBatchAggregateDailySql
                                                 : kBatchAggregateMinutesSql;
    Rows = parseTabSeparated(Client->query(Sql, Params));
  }

  for (const auto &Row : Rows) {
    requireColumns(Row, 9);
    const auto It = Result.find(Row[0]);
    if (It == Result.end()) {
      throw core::MarketDataError(
          "ClickHouse: неожиданный secid в пакетном ответе: " + Row[0]);
    }
    appendParsedBar(Row, 1, ToMs, It->second);
  }
  return Result;
}

std::vector<core::Bar>
ClickHouseBarSource::load(const core::BarRequest &Request) {
  validateIntervalSec(Request.IntervalSec);
  requireReadySnapshot(Request.SnapshotId);
  const auto Securities = loadSnapshotSecurities(Request.SnapshotId);
  (void)requireSecurity(*Securities, Request.Secid);
  if (Request.FromMs >= Request.ToMs)
    return {};

  const BarCacheKey Key{Request.SnapshotId, Request.Secid, Request.IntervalSec,
                        Request.FromMs, Request.ToMs};
  if (auto Cached = getCachedBars(Key))
    return *Cached;

  const auto Stored = loadStoredIntervals(Request.SnapshotId);
  const bool FromCandles =
      hasStoredInterval(*Stored, Request.Secid, Request.IntervalSec);
  auto Bars = std::make_shared<std::vector<core::Bar>>(
      fetchSingle(Request, FromCandles));
  putCachedBars(Key, Bars);
  return *Bars;
}

std::vector<std::vector<core::Bar>>
ClickHouseBarSource::loadBatch(const core::BarBatchRequest &Request) {
  validateIntervalSec(Request.IntervalSec);
  requireReadySnapshot(Request.SnapshotId);
  const auto Securities = loadSnapshotSecurities(Request.SnapshotId);
  for (const std::string &Secid : Request.Secids) {
    (void)requireSecurity(*Securities, Secid);
  }
  std::vector<std::vector<core::Bar>> Out(Request.Secids.size());
  if (Request.Secids.empty() || Request.FromMs >= Request.ToMs)
    return Out;

  std::unordered_map<std::string, std::shared_ptr<const std::vector<core::Bar>>>
      Resolved;
  Resolved.reserve(Request.Secids.size());
  std::vector<std::string> UniqueMisses;
  std::unordered_set<std::string> SeenMisses;

  for (const std::string &Secid : Request.Secids) {
    if (Resolved.contains(Secid) || SeenMisses.contains(Secid))
      continue;
    const BarCacheKey Key{Request.SnapshotId, Secid, Request.IntervalSec,
                          Request.FromMs, Request.ToMs};
    if (auto Cached = getCachedBars(Key)) {
      Resolved.emplace(Secid, std::move(Cached));
    } else {
      SeenMisses.insert(Secid);
      UniqueMisses.push_back(Secid);
    }
  }

  if (!UniqueMisses.empty()) {
    const auto Stored = loadStoredIntervals(Request.SnapshotId);
    std::vector<std::string> StoredMisses;
    std::vector<std::string> AggregatedMisses;
    for (const std::string &Secid : UniqueMisses) {
      if (hasStoredInterval(*Stored, Secid, Request.IntervalSec)) {
        StoredMisses.push_back(Secid);
      } else {
        AggregatedMisses.push_back(Secid);
      }
    }

    auto MergeFetched =
        [&](std::unordered_map<std::string, std::vector<core::Bar>> Fetched) {
          for (auto &[Secid, Series] : Fetched) {
            auto Ptr =
                std::make_shared<std::vector<core::Bar>>(std::move(Series));
            BarCacheKey Key{Request.SnapshotId, Secid, Request.IntervalSec,
                            Request.FromMs, Request.ToMs};
            putCachedBars(std::move(Key), Ptr);
            Resolved.emplace(Secid, std::move(Ptr));
          }
        };

    if (!StoredMisses.empty()) {
      MergeFetched(fetchBatch(Request.SnapshotId, StoredMisses,
                              Request.IntervalSec, Request.FromMs, Request.ToMs,
                              /*FromCandles=*/true));
    }
    if (!AggregatedMisses.empty()) {
      MergeFetched(fetchBatch(Request.SnapshotId, AggregatedMisses,
                              Request.IntervalSec, Request.FromMs, Request.ToMs,
                              /*FromCandles=*/false));
    }
  }

  for (std::size_t I = 0; I < Request.Secids.size(); ++I) {
    Out[I] = *Resolved.at(Request.Secids[I]);
  }
  return Out;
}

} // namespace worker
