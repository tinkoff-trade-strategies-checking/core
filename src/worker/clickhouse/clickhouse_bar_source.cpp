#include "clickhouse/clickhouse_bar_source.hpp"

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include "core/money/decimal.hpp"

namespace worker {

namespace {

constexpr std::int32_t kMaxTradeAggregationSec =
    86400; // предел candles_from_trades

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

std::int64_t parseInt64(const std::string &Text, const char *What) {
  std::int64_t Value = 0;
  const auto [ptr, ec] =
      std::from_chars(Text.data(), Text.data() + Text.size(), Value);
  if (ec != std::errc() || ptr != Text.data() + Text.size()) {
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

const char *const kTradesSql =
    "SELECT toUnixTimestamp64Milli(ts_open), "
    "toUnixTimestamp64Milli(available_at), "
    "open, high, low, close, volume, value "
    "FROM candles_from_trades(snapshot_id = {snapshot_id:UUID}, interval_sec = "
    "{interval_sec:UInt32}) "
    "WHERE secid = {secid:String} "
    "AND ts_open >= fromUnixTimestamp64Milli({from_ms:Int64}, 'UTC') "
    "AND ts_open < fromUnixTimestamp64Milli({to_ms:Int64}, 'UTC') "
    "AND available_at <= fromUnixTimestamp64Milli({to_ms:Int64}, 'UTC') "
    "ORDER BY ts_open ASC FORMAT TabSeparated";

} // namespace

ClickHouseBarSource::ClickHouseBarSource(
    std::unique_ptr<ClickHouseClient> HttpClient)
    : Client(std::move(HttpClient)) {
  if (!Client)
    throw std::invalid_argument("ClickHouseBarSource: клиент не задан");
}

ClickHouseBarSource::ClickHouseBarSource(const ClickHouseConfig &Config)
    : ClickHouseBarSource(std::make_unique<CurlClickHouseClient>(Config)) {}

void ClickHouseBarSource::requireReadySnapshot(const std::string &SnapshotId) {
  if (!looksLikeUuid(SnapshotId))
    throw core::SnapshotNotFound("версия данных не найдена: " + SnapshotId);
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
}

core::Security ClickHouseBarSource::security(const std::string &SnapshotId,
                                             const std::string &Secid) {
  requireReadySnapshot(SnapshotId);
  // Один secid на две записи (например, две площадки) - неоднозначно, выбирать
  // «первую» нельзя (INV-4).
  if (requireSecurity(SnapshotId, Secid) > 1) {
    throw core::MarketDataError(
        "в версии данных несколько записей инструмента: " + Secid);
  }
  const auto Rows = parseTabSeparated(Client->query(
      "SELECT secid, sector, lot_size, price_step FROM securities "
      "WHERE snapshot_id = {snapshot_id:UUID} AND secid = {secid:String} "
      "FORMAT TabSeparated",
      {{"snapshot_id", SnapshotId}, {"secid", escapeParam(Secid)}}));
  if (Rows.empty())
    throw core::SecurityNotFound("инструмент не найден: " + Secid);
  requireColumns(Rows[0], 4);
  core::Security Sec;
  Sec.Secid = Rows[0][0];
  Sec.Sector = Rows[0][1];
  Sec.LotSize = parseInt64(Rows[0][2], "lot_size");
  Sec.PriceStep = parseDecimal(Rows[0][3], "price_step");
  return Sec;
}

std::int64_t ClickHouseBarSource::requireSecurity(const std::string &SnapshotId,
                                                  const std::string &Secid) {
  const auto Rows = parseTabSeparated(Client->query(
      "SELECT count() FROM securities WHERE snapshot_id = {snapshot_id:UUID} "
      "AND secid = {secid:String} "
      "FORMAT TabSeparated",
      {{"snapshot_id", SnapshotId}, {"secid", escapeParam(Secid)}}));
  if (Rows.size() != 1 || Rows[0].size() != 1)
    throw core::MarketDataError("ClickHouse: неожиданный ответ securities");
  const std::int64_t Count = parseInt64(Rows[0][0], "count");
  if (Count == 0)
    throw core::SecurityNotFound("инструмент не найден: " + Secid);
  return Count;
}

std::vector<core::Bar>
ClickHouseBarSource::load(const core::BarRequest &Request) {
  if (Request.IntervalSec < 1)
    throw std::invalid_argument("BarRequest: intervalSec должен быть >= 1");
  requireReadySnapshot(Request.SnapshotId);
  if (requireSecurity(Request.SnapshotId, Request.Secid) > 1) {
    throw core::MarketDataError(
        "в версии данных несколько записей инструмента: " + Request.Secid);
  }
  if (Request.FromMs >= Request.ToMs)
    return {};

  const QueryParams Base{{"snapshot_id", Request.SnapshotId},
                         {"secid", escapeParam(Request.Secid)},
                         {"interval_sec", std::to_string(Request.IntervalSec)}};

  const auto Stored = parseTabSeparated(Client->query(
      "SELECT count() FROM candles WHERE snapshot_id = {snapshot_id:UUID} AND "
      "secid = {secid:String} "
      "AND interval_sec = {interval_sec:UInt32} FORMAT TabSeparated",
      Base));
  if (Stored.size() != 1 || Stored[0].size() != 1)
    throw core::MarketDataError("ClickHouse: неожиданный ответ candles");
  const bool FromCandles = parseInt64(Stored[0][0], "count") > 0;

  if (!FromCandles && Request.IntervalSec > kMaxTradeAggregationSec) {
    throw IntervalUnavailable("интервал " +
                              std::to_string(Request.IntervalSec) +
                              " с не хранится как свечи и не агрегируется из "
                              "сделок (максимум 86400)");
  }

  QueryParams Params = Base;
  Params.emplace_back("from_ms", std::to_string(Request.FromMs));
  Params.emplace_back("to_ms", std::to_string(Request.ToMs));
  const auto Rows = parseTabSeparated(
      Client->query(FromCandles ? kCandlesSql : kTradesSql, Params));

  std::vector<core::Bar> Bars;
  Bars.reserve(Rows.size());
  for (const auto &Row : Rows) {
    requireColumns(Row, 8);
    core::Bar Bar;
    Bar.Index = static_cast<std::int64_t>(Bars.size());
    Bar.TsOpenMs = parseInt64(Row[0], "ts_open");
    Bar.AvailableAtMs = parseInt64(Row[1], "available_at");
    Bar.Open = parseDecimal(Row[2], "open");
    Bar.High = parseDecimal(Row[3], "high");
    Bar.Low = parseDecimal(Row[4], "low");
    Bar.Close = parseDecimal(Row[5], "close");
    Bar.Volume = parseInt64(Row[6], "volume");
    Bar.Value = parseDecimal(Row[7], "value");
    // INV-1: бар, который к toMs ещё не стал известен, не отдаётся (сервер уже
    // отфильтровал; здесь защита).
    if (Bar.AvailableAtMs > Request.ToMs || Bar.AvailableAtMs < Bar.TsOpenMs) {
      throw core::MarketDataError("ClickHouse: бар с недопустимым available_at "
                                  "(позже toMs или раньше ts_open)");
    }
    if (!Bars.empty() && Bar.TsOpenMs <= Bars.back().TsOpenMs) {
      throw core::MarketDataError(
          "ClickHouse: бары не упорядочены по ts_open (дубликат в данных?)");
    }
    Bars.push_back(Bar);
  }
  return Bars;
}

} // namespace worker
