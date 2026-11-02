#include "clickhouse/clickhouse_calendar_source.hpp"

#include <charconv>
#include <stdexcept>
#include <utility>

namespace worker {

namespace {

// UUID в каноническом виде 8-4-4-4-12: иное сервер отвергнет ошибкой, а нужен SnapshotNotFound.
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

bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

bool isValidYmd(std::int32_t ymd) {
  const int y = ymd / 10000;
  const int m = ymd / 100 % 100;
  const int d = ymd % 100;
  if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1) return false;
  static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return d <= kDays[m - 1] + (m == 2 && isLeap(y) ? 1 : 0);
}

std::string formatDate(std::int32_t ymd) {
  if (!isValidYmd(ymd)) throw std::invalid_argument("неверная дата календаря: " + std::to_string(ymd));
  const std::string digits = std::to_string(ymd);  // ровно 8 цифр: год >= 1000 не гарантирован, добиваем нулями
  const std::string padded = std::string(8 - digits.size(), '0') + digits;
  return padded.substr(0, 4) + "-" + padded.substr(4, 2) + "-" + padded.substr(6, 2);
}

// 'YYYY-MM-DD' -> yyyymmdd; иное - ошибка данных.
std::int32_t parseDate(const std::string& text) {
  const auto bad = [&text] { return core::MarketDataError("ClickHouse: неверная дата календаря: " + text); };
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') throw bad();
  std::int32_t digits[3] = {0, 0, 0};
  const std::size_t starts[3] = {0, 5, 8};
  const std::size_t lens[3] = {4, 2, 2};
  for (int i = 0; i < 3; ++i) {
    const char* first = text.data() + starts[i];
    const auto [ptr, ec] = std::from_chars(first, first + lens[i], digits[i]);
    if (ec != std::errc() || ptr != first + lens[i]) throw bad();
  }
  const std::int32_t ymd = digits[0] * 10000 + digits[1] * 100 + digits[2];
  if (!isValidYmd(ymd)) throw bad();
  return ymd;
}

}  // namespace

ClickHouseCalendarSource::ClickHouseCalendarSource(std::unique_ptr<ClickHouseClient> client)
    : client_(std::move(client)) {
  if (!client_) throw std::invalid_argument("ClickHouseCalendarSource: клиент не задан");
}

ClickHouseCalendarSource::ClickHouseCalendarSource(const ClickHouseConfig& config)
    : ClickHouseCalendarSource(std::make_unique<CurlClickHouseClient>(config)) {}

std::vector<core::TradingDay> ClickHouseCalendarSource::tradingDays(const std::string& snapshotId,
                                                                    std::int32_t fromYmd, std::int32_t toYmd) {
  const std::string from = formatDate(fromYmd);
  const std::string to = formatDate(toYmd);
  if (!looksLikeUuid(snapshotId)) throw core::SnapshotNotFound("версия данных не найдена: " + snapshotId);

  const auto ready = parseTabSeparated(client_->query(
      "SELECT count() FROM snapshots_ready WHERE snapshot_id = {sid:UUID} FORMAT TabSeparated",
      {{"sid", snapshotId}}));
  if (ready.size() != 1 || ready[0].size() != 1) {
    throw core::MarketDataError("ClickHouse: неожиданный ответ snapshots_ready");
  }
  if (ready[0][0] == "0") throw core::SnapshotNotFound("версия данных не найдена или не готова: " + snapshotId);
  if (ready[0][0] != "1") throw core::MarketDataError("ClickHouse: неожиданный ответ snapshots_ready");

  if (fromYmd > toYmd) return {};

  const auto rows = parseTabSeparated(client_->query(
      "SELECT toString(date), is_trading FROM trading_calendar "
      "WHERE snapshot_id = {sid:UUID} AND date BETWEEN {f:Date} AND {t:Date} "
      "ORDER BY date ASC FORMAT TabSeparated",
      {{"sid", snapshotId}, {"f", from}, {"t", to}}));

  std::vector<core::TradingDay> days;
  days.reserve(rows.size());
  for (const auto& row : rows) {
    if (row.size() != 2) throw core::MarketDataError("ClickHouse: неожиданное число столбцов календаря");
    core::TradingDay day;
    day.yyyymmdd = parseDate(row[0]);
    if (row[1] != "0" && row[1] != "1") {
      throw core::MarketDataError("ClickHouse: неверный is_trading в календаре: " + row[1]);
    }
    day.isTrading = row[1] == "1";
    if (day.yyyymmdd < fromYmd || day.yyyymmdd > toYmd) {
      throw core::MarketDataError("ClickHouse: дата календаря вне запрошенного диапазона");
    }
    if (!days.empty() && day.yyyymmdd <= days.back().yyyymmdd) {
      throw core::MarketDataError("ClickHouse: даты календаря не упорядочены (дубликат в данных?)");
    }
    days.push_back(day);
  }
  return days;
}

}  // namespace worker
