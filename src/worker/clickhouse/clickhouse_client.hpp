#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/market/bar_source.hpp"

// Тонкий клиент HTTP-интерфейса ClickHouse (порт 8123, libcurl). Только чтение: каждый запрос
// уходит с readonly=1. Значения подставляются через параметризованные запросы
// (`{name:Type}` в SQL + `param_name` в URL), а не склейкой строк.

namespace worker {

class ClickHouseError : public core::MarketDataError {
 public:
  using core::MarketDataError::MarketDataError;
};

// Запрошенный интервал свечей в версии данных недоступен (не хранится и не агрегируется из сделок). Единственная
// ошибка данных, которую пользователь исправляет сам (другой таймфрейм): воркер отвечает NOT_FOUND. Остальные
// MarketDataError - повреждённые или неожиданные данные хранилища (INTERNAL).
class IntervalUnavailable : public core::MarketDataError {
 public:
  using core::MarketDataError::MarketDataError;
};

struct ClickHouseConfig {
  std::string baseUrl = "http://127.0.0.1:8123";  // без пути и учётных данных
  std::string database = "default";
  std::string user;
  std::string password;
  long timeoutSec = 60;

  // Разбор адреса вида http[s]://[user[:password]@]host[:port][/database] (как у импортёра
  // `marketdata-import`, `$CLICKHOUSE_URL`); user и password декодируются из %XX.
  // Неверный адрес - std::invalid_argument.
  static ClickHouseConfig fromUrl(const std::string& url);
};

// Именованные параметры запроса: (имя без префикса param_, значение в текстовом виде).
using QueryParams = std::vector<std::pair<std::string, std::string>>;

// Абстракция клиента: боевая реализация - libcurl, в тестах - подмена.
class ClickHouseClient {
 public:
  virtual ~ClickHouseClient() = default;
  // Выполняет запрос и возвращает тело ответа как есть (формат задаёт сам SQL через FORMAT).
  // Любая ошибка транспорта или статус не 200 - ClickHouseError.
  virtual std::string query(const std::string& sql, const QueryParams& params) = 0;
};

class CurlClickHouseClient final : public ClickHouseClient {
 public:
  explicit CurlClickHouseClient(ClickHouseConfig config);
  std::string query(const std::string& sql, const QueryParams& params) override;

 private:
  ClickHouseConfig config_;
};

// Разбор ответа FORMAT TabSeparated: строки разделены \n, поля - \t, экранирование ClickHouse
// (\t \n \\ \' \0 и др.) снимается. Пустой ответ - ноль строк; непустой ответ без завершающего \n считается оборванным (ClickHouseError).
std::vector<std::vector<std::string>> parseTabSeparated(const std::string& body);

}  // namespace worker
