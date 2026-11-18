#include "clickhouse/clickhouse_ruonia_source.hpp"

#include <charconv>
#include <stdexcept>
#include <utility>

namespace worker {

namespace {

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

bool isLeap(int Y) { return (Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0; }

bool isValidYmd(std::int32_t Ymd) {
  const int Y = Ymd / 10000;
  const int M = Ymd / 100 % 100;
  const int D = Ymd % 100;
  if (Y < 1 || Y > 9999 || M < 1 || M > 12 || D < 1)
    return false;
  static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30,
                                    31, 31, 30, 31, 30, 31};
  return D <= kDays[M - 1] + (M == 2 && isLeap(Y) ? 1 : 0);
}

std::string formatDate(std::int32_t Ymd) {
  if (!isValidYmd(Ymd))
    throw std::invalid_argument("неверная дата RUONIA: " + std::to_string(Ymd));
  const std::string Digits = std::to_string(Ymd);
  const std::string Padded = std::string(8 - Digits.size(), '0') + Digits;
  return Padded.substr(0, 4) + "-" + Padded.substr(4, 2) + "-" +
         Padded.substr(6, 2);
}

std::int32_t parseDate(const std::string &Text) {
  const auto Bad = [&Text] {
    return core::MarketDataError("ClickHouse: неверная дата RUONIA: " + Text);
  };
  if (Text.size() != 10 || Text[4] != '-' || Text[7] != '-')
    throw Bad();
  std::int32_t Digits[3] = {0, 0, 0};
  const std::size_t Starts[3] = {0, 5, 8};
  const std::size_t Lens[3] = {4, 2, 2};
  for (int I = 0; I < 3; ++I) {
    const char *First = Text.data() + Starts[I];
    const auto [Ptr, Ec] = std::from_chars(First, First + Lens[I], Digits[I]);
    if (Ec != std::errc() || Ptr != First + Lens[I])
      throw Bad();
  }
  const std::int32_t Ymd = Digits[0] * 10000 + Digits[1] * 100 + Digits[2];
  if (!isValidYmd(Ymd))
    throw Bad();
  return Ymd;
}

core::Decimal parseRate(const std::string &Text) {
  core::Decimal Rate;
  try {
    Rate = core::Decimal::parse(Text);
  } catch (const std::exception &) {
    throw core::MarketDataError("ClickHouse: неверная ставка RUONIA: " + Text);
  }
  if (Rate <= core::Decimal::zero()) {
    throw core::MarketDataError(
        "ClickHouse: ставка RUONIA должна быть положительной: " + Text);
  }
  return Rate;
}

} // namespace

ClickHouseRuoniaSource::ClickHouseRuoniaSource(
    std::unique_ptr<ClickHouseClient> HttpClient)
    : Client(std::move(HttpClient)) {
  if (!Client)
    throw std::invalid_argument("ClickHouseRuoniaSource: клиент не задан");
}

ClickHouseRuoniaSource::ClickHouseRuoniaSource(const ClickHouseConfig &Config)
    : ClickHouseRuoniaSource(std::make_unique<CurlClickHouseClient>(Config)) {}

std::vector<core::DailyRate>
ClickHouseRuoniaSource::ruoniaRates(const std::string &SnapshotId,
                                    std::int32_t UpToYmd) {
  const std::string UpTo = formatDate(UpToYmd);
  if (!looksLikeUuid(SnapshotId))
    throw core::SnapshotNotFound("версия данных не найдена: " + SnapshotId);

  const auto Ready = parseTabSeparated(
      Client->query("SELECT count() FROM snapshots_ready WHERE snapshot_id = "
                    "{sid:UUID} FORMAT TabSeparated",
                    {{"sid", SnapshotId}}));
  if (Ready.size() != 1 || Ready[0].size() != 1) {
    throw core::MarketDataError(
        "ClickHouse: неожиданный ответ snapshots_ready");
  }
  if (Ready[0][0] == "0")
    throw core::SnapshotNotFound("версия данных не найдена или не готова: " +
                                 SnapshotId);
  if (Ready[0][0] != "1")
    throw core::MarketDataError(
        "ClickHouse: неожиданный ответ snapshots_ready");

  const auto Rows = parseTabSeparated(
      Client->query("SELECT toString(date), toString(rate) FROM ruonia "
                    "WHERE snapshot_id = {sid:UUID} AND date <= {t:Date} "
                    "ORDER BY date ASC FORMAT TabSeparated",
                    {{"sid", SnapshotId}, {"t", UpTo}}));

  std::vector<core::DailyRate> Out;
  Out.reserve(Rows.size());
  for (const auto &Row : Rows) {
    if (Row.size() != 2)
      throw core::MarketDataError(
          "ClickHouse: неожиданное число столбцов ruonia");
    core::DailyRate Rate;
    Rate.Yyyymmdd = parseDate(Row[0]);
    Rate.Rate = parseRate(Row[1]);
    if (Rate.Yyyymmdd > UpToYmd)
      throw core::MarketDataError(
          "ClickHouse: дата RUONIA позже запрошенной: " + Row[0]);
    if (!Out.empty() && Rate.Yyyymmdd <= Out.back().Yyyymmdd) {
      throw core::MarketDataError(
          "ClickHouse: даты RUONIA не упорядочены (дубликат в данных?)");
    }
    Out.push_back(Rate);
  }
  return Out;
}

} // namespace worker
