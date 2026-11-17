#include "clickhouse/clickhouse_client.hpp"

#include <curl/curl.h>

#include <cctype>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace worker {

namespace {

std::string percentDecode(const std::string &text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() &&
        std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
      out.push_back(
          static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)));
      i += 2;
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

void ensureCurlGlobalInit() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::size_t appendToString(char *data, std::size_t size, std::size_t count,
                           void *userdata) {
  static_cast<std::string *>(userdata)->append(data, size * count);
  return size * count;
}

struct CurlDeleter {
  void operator()(CURL *h) const { curl_easy_cleanup(h); }
};
struct SlistDeleter {
  void operator()(curl_slist *l) const { curl_slist_free_all(l); }
};

std::string escapeUrl(CURL *handle, const std::string &text) {
  char *escaped =
      curl_easy_escape(handle, text.c_str(), static_cast<int>(text.size()));
  if (escaped == nullptr)
    throw ClickHouseError("curl_easy_escape: нехватка памяти");
  std::string out(escaped);
  curl_free(escaped);
  return out;
}

std::string trimmed(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    text.pop_back();
  if (text.size() > 500)
    text.resize(500);
  return text;
}

} // namespace

ClickHouseConfig ClickHouseConfig::fromUrl(const std::string &url) {
  const auto schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos)
    throw std::invalid_argument("ClickHouse URL без схемы (ожидается "
                                "http://[user:password@]host[:port][/база])");
  const std::string scheme = url.substr(0, schemeEnd);
  if (scheme != "http" && scheme != "https") {
    throw std::invalid_argument(
        "ClickHouse URL: допустимы только http и https");
  }
  const std::string rest = url.substr(schemeEnd + 3);
  const auto pathStart = rest.find('/');
  const std::string authority = rest.substr(0, pathStart);
  const std::string path =
      pathStart == std::string::npos ? "" : rest.substr(pathStart + 1);

  ClickHouseConfig config;
  std::string hostPort = authority;
  const auto at = authority.rfind('@');
  if (at != std::string::npos) {
    const std::string userInfo = authority.substr(0, at);
    hostPort = authority.substr(at + 1);
    const auto colon = userInfo.find(':');
    config.user = percentDecode(userInfo.substr(0, colon));
    if (colon != std::string::npos)
      config.password = percentDecode(userInfo.substr(colon + 1));
  }
  if (hostPort.empty())
    throw std::invalid_argument("ClickHouse URL без хоста");
  config.baseUrl = scheme + "://" + hostPort;
  if (!path.empty())
    config.database = percentDecode(path.substr(0, path.find('?')));
  return config;
}

CurlClickHouseClient::CurlClickHouseClient(ClickHouseConfig config)
    : config_(std::move(config)) {
  ensureCurlGlobalInit();
}

std::string CurlClickHouseClient::query(const std::string &sql,
                                        const QueryParams &params) {
  std::unique_ptr<CURL, CurlDeleter> handle(curl_easy_init());
  if (!handle)
    throw ClickHouseError("curl_easy_init не удался");

  std::string url = config_.baseUrl +
                    "/?readonly=1&wait_end_of_query=1&database=" +
                    escapeUrl(handle.get(), config_.database);
  for (const auto &[name, value] : params) {
    url += "&param_" + escapeUrl(handle.get(), name) + "=" +
           escapeUrl(handle.get(), value);
  }

  std::string response;
  std::unique_ptr<curl_slist, SlistDeleter> headers;
  if (!config_.user.empty()) {
    curl_slist *list = nullptr;
    for (const std::string &line : {"X-ClickHouse-User: " + config_.user,
                                    "X-ClickHouse-Key: " + config_.password}) {
      curl_slist *extended = curl_slist_append(list, line.c_str());
      if (extended == nullptr) {
        curl_slist_free_all(list);
        throw ClickHouseError("curl_slist_append: нехватка памяти");
      }
      list = extended;
    }
    headers.reset(list);
  }

  CURL *h = handle.get();
  curl_easy_setopt(h, CURLOPT_URL, url.c_str());
  curl_easy_setopt(h, CURLOPT_POST, 1L);
  curl_easy_setopt(h, CURLOPT_POSTFIELDS, sql.c_str());
  curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE,
                   static_cast<curl_off_t>(sql.size()));
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, appendToString);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(h, CURLOPT_TIMEOUT, config_.timeoutSec);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  if (headers)
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers.get());

  const CURLcode rc = curl_easy_perform(h);
  if (rc != CURLE_OK) {
    throw ClickHouseError(std::string("ClickHouse недоступен: ") +
                          curl_easy_strerror(rc));
  }
  long status = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  if (status != 200) {
    throw ClickHouseError("ClickHouse вернул HTTP " + std::to_string(status) +
                          ": " + trimmed(response));
  }
  return response;
}

std::vector<std::vector<std::string>>
parseTabSeparated(const std::string &body) {
  std::vector<std::vector<std::string>> rows;
  if (body.empty())
    return rows;
  // Каждая строка ответа заканчивается \n; иначе ответ оборван, и последняя
  // строка может быть неполной.
  if (body.back() != '\n')
    throw ClickHouseError(
        "ClickHouse: ответ оборван (нет завершающего перевода строки)");
  std::vector<std::string> row;
  std::string field;
  for (std::size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (c == '\t') {
      row.push_back(std::move(field));
      field.clear();
    } else if (c == '\n') {
      row.push_back(std::move(field));
      field.clear();
      rows.push_back(std::move(row));
      row.clear();
    } else if (c == '\\' && i + 1 < body.size()) {
      const char n = body[++i];
      switch (n) {
      case 't':
        field.push_back('\t');
        break;
      case 'n':
        field.push_back('\n');
        break;
      case 'r':
        field.push_back('\r');
        break;
      case 'b':
        field.push_back('\b');
        break;
      case 'f':
        field.push_back('\f');
        break;
      case 'a':
        field.push_back('\a');
        break;
      case 'v':
        field.push_back('\v');
        break;
      case '0':
        field.push_back('\0');
        break;
      default:
        field.push_back(n);
        break; // \\ \' и прочее
      }
    } else {
      field.push_back(c);
    }
  }
  return rows;
}

} // namespace worker
