#include "clickhouse/clickhouse_client.hpp"

#include <curl/curl.h>

#include <cctype>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace worker {

namespace {

std::string percentDecode(const std::string &Text) {
  std::string Out;
  for (std::size_t I = 0; I < Text.size(); ++I) {
    if (Text[I] == '%' && I + 2 < Text.size() &&
        std::isxdigit(static_cast<unsigned char>(Text[I + 1])) &&
        std::isxdigit(static_cast<unsigned char>(Text[I + 2]))) {
      Out.push_back(
          static_cast<char>(std::stoi(Text.substr(I + 1, 2), nullptr, 16)));
      I += 2;
    } else {
      Out.push_back(Text[I]);
    }
  }
  return Out;
}

void ensureCurlGlobalInit() {
  static std::once_flag Once;
  std::call_once(Once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::size_t appendToString(char *Data, std::size_t Size, std::size_t Count,
                           void *Userdata) {
  static_cast<std::string *>(Userdata)->append(Data, Size * Count);
  return Size * Count;
}

struct CurlDeleter {
  void operator()(CURL *H) const { curl_easy_cleanup(H); }
};
struct SlistDeleter {
  void operator()(curl_slist *L) const { curl_slist_free_all(L); }
};

constexpr std::size_t kMaxPooledHandles = 32;

std::string escapeUrl(CURL *Handle, const std::string &Text) {
  char *Escaped =
      curl_easy_escape(Handle, Text.c_str(), static_cast<int>(Text.size()));
  if (Escaped == nullptr)
    throw ClickHouseError("curl_easy_escape: нехватка памяти");
  std::string Out(Escaped);
  curl_free(Escaped);
  return Out;
}

std::string trimmed(std::string Text) {
  while (!Text.empty() && (Text.back() == '\n' || Text.back() == '\r'))
    Text.pop_back();
  if (Text.size() > 500)
    Text.resize(500);
  return Text;
}

} // namespace

ClickHouseConfig ClickHouseConfig::fromUrl(const std::string &Url) {
  const auto SchemeEnd = Url.find("://");
  if (SchemeEnd == std::string::npos)
    throw std::invalid_argument("ClickHouse URL без схемы (ожидается "
                                "http://[user:password@]host[:port][/база])");
  const std::string Scheme = Url.substr(0, SchemeEnd);
  if (Scheme != "http" && Scheme != "https") {
    throw std::invalid_argument(
        "ClickHouse URL: допустимы только http и https");
  }
  const std::string Rest = Url.substr(SchemeEnd + 3);
  const auto PathStart = Rest.find('/');
  const std::string Authority = Rest.substr(0, PathStart);
  const std::string Path =
      PathStart == std::string::npos ? "" : Rest.substr(PathStart + 1);

  ClickHouseConfig Config;
  std::string HostPort = Authority;
  const auto At = Authority.rfind('@');
  if (At != std::string::npos) {
    const std::string UserInfo = Authority.substr(0, At);
    HostPort = Authority.substr(At + 1);
    const auto Colon = UserInfo.find(':');
    Config.User = percentDecode(UserInfo.substr(0, Colon));
    if (Colon != std::string::npos)
      Config.Password = percentDecode(UserInfo.substr(Colon + 1));
  }
  if (HostPort.empty())
    throw std::invalid_argument("ClickHouse URL без хоста");
  Config.BaseUrl = Scheme + "://" + HostPort;
  if (!Path.empty())
    Config.Database = percentDecode(Path.substr(0, Path.find('?')));
  return Config;
}

struct CurlClickHouseClient::HandlePool {
  std::mutex Mutex;
  std::vector<std::unique_ptr<CURL, CurlDeleter>> Handles;

  std::unique_ptr<CURL, CurlDeleter> acquire() {
    {
      std::lock_guard<std::mutex> Lock(Mutex);
      if (!Handles.empty()) {
        auto Handle = std::move(Handles.back());
        Handles.pop_back();
        return Handle;
      }
    }
    std::unique_ptr<CURL, CurlDeleter> Handle(curl_easy_init());
    if (!Handle)
      throw ClickHouseError("curl_easy_init не удался");
    return Handle;
  }

  void release(std::unique_ptr<CURL, CurlDeleter> Handle) {
    if (!Handle)
      return;
    curl_easy_reset(Handle.get());
    std::lock_guard<std::mutex> Lock(Mutex);
    if (Handles.size() < kMaxPooledHandles) {
      Handles.push_back(std::move(Handle));
    }
  }
};

CurlClickHouseClient::CurlClickHouseClient(ClickHouseConfig Settings)
    : Config(std::move(Settings)), Pool(std::make_unique<HandlePool>()) {
  ensureCurlGlobalInit();
}

CurlClickHouseClient::~CurlClickHouseClient() = default;

std::string CurlClickHouseClient::query(const std::string &Sql,
                                        const QueryParams &Params) {
  std::unique_ptr<CURL, CurlDeleter> Handle = Pool->acquire();

  std::string Url = Config.BaseUrl +
                    "/?readonly=1&wait_end_of_query=1&database=" +
                    escapeUrl(Handle.get(), Config.Database);
  for (const auto &[Name, Value] : Params) {
    Url += "&param_" + escapeUrl(Handle.get(), Name) + "=" +
           escapeUrl(Handle.get(), Value);
  }

  std::string Response;
  std::unique_ptr<curl_slist, SlistDeleter> Headers;
  {
    // Отключаем заголовок Expect: 100-continue, чтобы крупные POST-запросы
    // (например, batch secid IN (...)) не ждали 1 с ответа сервера.
    curl_slist *List = curl_slist_append(nullptr, "Expect:");
    if (List == nullptr)
      throw ClickHouseError("curl_slist_append: нехватка памяти");
    if (!Config.User.empty()) {
      for (const std::string &Line : {"X-ClickHouse-User: " + Config.User,
                                      "X-ClickHouse-Key: " + Config.Password}) {
        curl_slist *Extended = curl_slist_append(List, Line.c_str());
        if (Extended == nullptr) {
          curl_slist_free_all(List);
          throw ClickHouseError("curl_slist_append: нехватка памяти");
        }
        List = Extended;
      }
    }
    Headers.reset(List);
  }

  CURL *H = Handle.get();
  curl_easy_setopt(H, CURLOPT_URL, Url.c_str());
  curl_easy_setopt(H, CURLOPT_POST, 1L);
  curl_easy_setopt(H, CURLOPT_POSTFIELDS, Sql.c_str());
  curl_easy_setopt(H, CURLOPT_POSTFIELDSIZE_LARGE,
                   static_cast<curl_off_t>(Sql.size()));
  curl_easy_setopt(H, CURLOPT_WRITEFUNCTION, appendToString);
  curl_easy_setopt(H, CURLOPT_WRITEDATA, &Response);
  curl_easy_setopt(H, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(H, CURLOPT_TIMEOUT, Config.TimeoutSec);
  curl_easy_setopt(H, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(H, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(H, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(H, CURLOPT_TCP_KEEPIDLE, 60L);
  curl_easy_setopt(H, CURLOPT_TCP_KEEPINTVL, 30L);
  curl_easy_setopt(H, CURLOPT_HTTPHEADER, Headers.get());

  const CURLcode Rc = curl_easy_perform(H);
  if (Rc != CURLE_OK) {
    // При транспортной ошибке не возвращаем потенциально повреждённое
    // соединение в пул.
    throw ClickHouseError(std::string("ClickHouse недоступен: ") +
                          curl_easy_strerror(Rc));
  }
  long Status = 0;
  curl_easy_getinfo(H, CURLINFO_RESPONSE_CODE, &Status);
  Pool->release(std::move(Handle));
  if (Status != 200) {
    throw ClickHouseError("ClickHouse вернул HTTP " + std::to_string(Status) +
                          ": " + trimmed(Response));
  }
  return Response;
}

std::vector<std::vector<std::string>>
parseTabSeparated(const std::string &Body) {
  std::vector<std::vector<std::string>> Rows;
  if (Body.empty())
    return Rows;
  // Каждая строка ответа заканчивается \n; иначе ответ оборван, и последняя
  // строка может быть неполной.
  if (Body.back() != '\n')
    throw ClickHouseError(
        "ClickHouse: ответ оборван (нет завершающего перевода строки)");
  std::vector<std::string> Row;
  std::string Field;
  for (std::size_t I = 0; I < Body.size(); ++I) {
    const char C = Body[I];
    if (C == '\t') {
      Row.push_back(std::move(Field));
      Field.clear();
    } else if (C == '\n') {
      Row.push_back(std::move(Field));
      Field.clear();
      Rows.push_back(std::move(Row));
      Row.clear();
    } else if (C == '\\' && I + 1 < Body.size()) {
      const char N = Body[++I];
      switch (N) {
      case 't':
        Field.push_back('\t');
        break;
      case 'n':
        Field.push_back('\n');
        break;
      case 'r':
        Field.push_back('\r');
        break;
      case 'b':
        Field.push_back('\b');
        break;
      case 'f':
        Field.push_back('\f');
        break;
      case 'a':
        Field.push_back('\a');
        break;
      case 'v':
        Field.push_back('\v');
        break;
      case '0':
        Field.push_back('\0');
        break;
      default:
        Field.push_back(N);
        break; // \\ \' и прочее
      }
    } else {
      Field.push_back(C);
    }
  }
  return Rows;
}

} // namespace worker
