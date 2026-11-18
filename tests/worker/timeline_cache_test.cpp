#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "convert/market.hpp"
#include "core/market/bar_source.hpp"
#include "core/market/timeline.hpp"
#include "handlers.hpp"
#include "timeline_cache.hpp"
#include "worker_test_support.hpp"

namespace {

using namespace worker_test;

// Обёртка над BarSource для подсчёта вызовов load().
class CountingBarSource final : public core::BarSource {
public:
  explicit CountingBarSource(core::BarSource &TargetSource)
      : Source(TargetSource) {}

  std::vector<core::Bar> load(const core::BarRequest &Request) override {
    ++LoadCount;
    return Source.load(Request);
  }

  core::Security security(const std::string &SnapshotId,
                          const std::string &Secid) override {
    return Source.security(SnapshotId, Secid);
  }

  std::int64_t loadCount() const { return LoadCount; }

private:
  core::BarSource &Source;
  std::int64_t LoadCount = 0;
};

// Создание фикстурной шкалы для тестов кэша.
std::shared_ptr<const core::Timeline> makeSampleTimeline() {
  core::testing::Fixture F = core::testing::fixture1();
  const auto Inst =
      core::Instrument{"A", "FIXA", 10, core::Decimal::parse("0.01")};
  return std::make_shared<const core::Timeline>(
      core::Timeline::load(F.Bars, F.Calendar, "fixture-1", {Inst},
                           core::Timeframe::D1, 20240109, 20240122));
}

worker::TimelineKey sampleKey(const std::string &SnapshotId = "fixture-1",
                              std::int32_t From = 20240109,
                              std::int32_t To = 20240122) {
  const auto Inst =
      core::Instrument{"A", "FIXA", 10, core::Decimal::parse("0.01")};
  return worker::TimelineKey{
      .SnapshotId = SnapshotId,
      .Instruments = {Inst},
      .Timeframe = core::Timeframe::D1,
      .FromYmd = From,
      .ToYmd = To,
      .WarmupBars = 0,
  };
}

} // namespace

// Проверка базового сохранения и получения шкалы по ключу.
TEST(TimelineCache, PutAndGet) {
  worker::TimelineCache Cache(4);
  EXPECT_EQ(Cache.size(), 0);
  EXPECT_EQ(Cache.capacity(), 4);

  const auto Key = sampleKey();
  EXPECT_EQ(Cache.get(Key), nullptr);

  const auto Ptr = makeSampleTimeline();
  Cache.put(Key, Ptr);

  EXPECT_EQ(Cache.size(), 1);
  const auto Found = Cache.get(Key);
  ASSERT_NE(Found, nullptr);
  EXPECT_EQ(Found, Ptr);
  EXPECT_EQ(Found->totalSteps(), Ptr->totalSteps());
}

// Различные ключи дают разные записи в кэше: различимо каждое поле ключа.
TEST(TimelineCache, DifferentKeysAreDistinguished) {
  worker::TimelineCache Cache(32);
  const auto Ptr = makeSampleTimeline();

  const worker::TimelineKey Base = sampleKey();
  std::vector<worker::TimelineKey> Variants(8, Base);
  Variants[1].SnapshotId = "snap-2";
  Variants[2].FromYmd = 20240110;
  Variants[3].ToYmd = 20240119;
  Variants[4].WarmupBars = 3;
  Variants[5].Timeframe = core::Timeframe::H1;
  Variants[6].Instruments.front().Ticker = "FIXZ";
  Variants[7].Instruments.push_back(
      core::Instrument{"B", "FIXB", 1, core::Decimal::parse("0.01")});

  // Только Base в кэше: ни один вариант его не находит.
  Cache.put(Base, Ptr);
  for (std::size_t I = 1; I < Variants.size(); ++I)
    EXPECT_EQ(Cache.get(Variants[I]), nullptr) << "вариант " << I;

  // Порядок бумаг - часть ключа.
  worker::TimelineKey Ab = Variants[7];
  worker::TimelineKey Ba = Ab;
  std::swap(Ba.Instruments.front(), Ba.Instruments.back());
  Cache.put(Ab, Ptr);
  EXPECT_EQ(Cache.get(Ba), nullptr);

  for (std::size_t I = 1; I < Variants.size(); ++I)
    Cache.put(Variants[I], Ptr);
  EXPECT_EQ(Cache.size(), 8);
  for (const auto &V : Variants)
    EXPECT_NE(Cache.get(V), nullptr);
}

// Вытеснение по LRU при превышении ёмкости кэша.
TEST(TimelineCache, LruEvictionPolicy) {
  worker::TimelineCache Cache(2); // Емкость 2
  const auto Ptr = makeSampleTimeline();

  const auto K1 = sampleKey("s1");
  const auto K2 = sampleKey("s2");
  const auto K3 = sampleKey("s3");

  Cache.put(K1, Ptr);
  Cache.put(K2, Ptr);
  EXPECT_EQ(Cache.size(), 2);

  // Обращаемся к K1, делая его MRU (наиболее свежим), а K2 остаётся LRU.
  EXPECT_NE(Cache.get(K1), nullptr);

  // Вставка K3 должна вытеснить наименее используемый K2.
  Cache.put(K3, Ptr);
  EXPECT_EQ(Cache.size(), 2);

  EXPECT_NE(Cache.get(K1), nullptr); // K1 остался
  EXPECT_EQ(Cache.get(K2), nullptr); // K2 вытеснен
  EXPECT_NE(Cache.get(K3), nullptr); // K3 на месте
}

// Очистка кэша.
TEST(TimelineCache, Clear) {
  worker::TimelineCache Cache(4);
  const auto Ptr = makeSampleTimeline();

  Cache.put(sampleKey("s1"), Ptr);
  Cache.put(sampleKey("s2"), Ptr);
  EXPECT_EQ(Cache.size(), 2);

  Cache.clear();
  EXPECT_EQ(Cache.size(), 0);
  EXPECT_EQ(Cache.get(sampleKey("s1")), nullptr);
}

// Потокобезопасность при параллельном доступе.
TEST(TimelineCache, ConcurrentAccess) {
  worker::TimelineCache Cache(8);
  const auto Ptr = makeSampleTimeline();

  std::vector<std::thread> Threads;
  for (int I = 0; I < 8; ++I) {
    Threads.emplace_back([&Cache, &Ptr, I] {
      const auto Key = sampleKey("snap-" + std::to_string(I % 4));
      for (int Step = 0; Step < 100; ++Step) {
        Cache.put(Key, Ptr);
        auto Res = Cache.get(Key);
        (void)Res;
      }
    });
  }

  for (auto &T : Threads) {
    T.join();
  }

  EXPECT_LE(Cache.size(), 8);
}

// При повторном вызове handleReplaySession шкала берётся из кэша без
// повторного чтения баров из источника данных.
TEST(TimelineCache, ReplaySessionReusesCachedTimeline) {
  core::testing::Fixture F = core::testing::fixture1();
  CountingBarSource CountingBars(F.Bars);
  worker::TimelineCache Cache(4);
  worker::Context Context{CountingBars, F.Calendar, F.Ruonia, &Cache};

  const pb::MarketSpec Market = fixtureMarket();
  pb::ReplaySessionRequest Request;
  *Request.mutable_market() = Market;
  Request.set_up_to_step(3);

  // Первый вызов: кэш-промах, читаем из источника
  pb::ReplaySessionResponse Resp1;
  const auto Status1 = worker::handleReplaySession(Context, Request, &Resp1);
  ASSERT_TRUE(Status1.ok());
  const std::int64_t InitialLoads = CountingBars.loadCount();
  EXPECT_GT(InitialLoads, 0);
  EXPECT_EQ(Cache.size(), 1);

  // Второй вызов с теми же параметрами рынка: кэш-хит, повторных вызовов load()
  // нет!
  pb::ReplaySessionResponse Resp2;
  const auto Status2 = worker::handleReplaySession(Context, Request, &Resp2);
  ASSERT_TRUE(Status2.ok());
  EXPECT_EQ(CountingBars.loadCount(), InitialLoads); // 0 новых вызовов load!
  EXPECT_EQ(Resp1.total_steps(), Resp2.total_steps());
}

namespace {

// Бенчмарк-бумага IMOEX на дневных барах A из fixture-1.
void addImoex(core::testing::Fixture &F) {
  std::vector<core::Bar> Imoex = F.Bars.load(core::BarRequest{
      "fixture-1", "FIXA", 86400, 0, std::numeric_limits<std::int64_t>::max()});
  for (std::size_t I = 0; I < Imoex.size(); ++I) {
    Imoex[I].Open =
        core::Decimal::fromUnits(1000 + 10 * static_cast<std::int64_t>(I));
    Imoex[I].Close =
        core::Decimal::fromUnits(1005 + 10 * static_cast<std::int64_t>(I));
  }
  F.Bars.addSecurity("fixture-1", core::Security{"IMOEX", "", 1,
                                                 core::Decimal::parse("0.01")});
  F.Bars.addSeries("fixture-1", "IMOEX", 86400, Imoex);
}

pb::JournalEntry limitOrder(std::int64_t Seq, std::int64_t Step,
                            const char *Key, pb::Side Side, std::int64_t Lots,
                            const char *Price) {
  pb::JournalEntry E = marketOrder(Seq, Step, Key, Side, Lots);
  pb::OrderSpec *Spec = E.mutable_place()->mutable_order();
  Spec->set_type(pb::LIMIT);
  *Spec->mutable_limit_price() = dec(Price);
  return E;
}

} // namespace

// INV-4: ответы с кэшем (промах и попадание) совпадают побайтово с ответами
// без кэша: журнал с заявками, прогрев, история, отчёт с бенчмарком.
TEST(TimelineCache, ReplaySessionResultEqualsUncached) {
  core::testing::Fixture F = core::testing::fixture1();
  addImoex(F);
  pb::MarketSpec Market = fixtureMarket();
  Market.set_from_date("2024-01-11");
  Market.set_warmup_bars(2);
  Market.set_benchmark_ticker("IMOEX");

  std::vector<pb::ReplaySessionRequest> Requests;
  for (const std::int64_t UpTo : {0, 3, 4, 6}) {
    pb::ReplaySessionRequest R;
    *R.mutable_market() = Market;
    // Записи журнала - не позже up_to_step.
    std::vector<pb::JournalEntry> Journal = {
        marketOrder(0, 0, "A", pb::BUY, 5),
        limitOrder(1, 1, "A", pb::BUY, 2, "50.00"),
        marketOrder(2, 2, "A", pb::SELL, 3)};
    for (const pb::JournalEntry &E : Journal)
      if (E.step() <= UpTo)
        *R.add_journal() = E;
    R.set_up_to_step(UpTo);
    R.set_events_from_step(-1);
    R.set_include_history(true);
    R.set_include_report(true);
    Requests.push_back(R);
  }

  worker::Context Plain{F.Bars, F.Calendar, F.Ruonia};
  std::vector<std::string> Expected;
  for (const auto &R : Requests) {
    pb::ReplaySessionResponse Resp;
    const auto Status = worker::handleReplaySession(Plain, R, &Resp);
    ASSERT_TRUE(Status.ok()) << Status.error_message();
    EXPECT_GT(Resp.warmup_steps(), 0);
    EXPECT_TRUE(Resp.report().metrics().has_benchmark_return());
    Expected.push_back(Resp.SerializeAsString());
  }

  worker::TimelineCache Cache(8);
  worker::Context Cached{F.Bars, F.Calendar, F.Ruonia, &Cache};
  for (int Pass = 0; Pass < 2; ++Pass) { // 0 - промахи, 1 - попадания
    for (std::size_t I = 0; I < Requests.size(); ++I) {
      pb::ReplaySessionResponse Resp;
      ASSERT_TRUE(worker::handleReplaySession(Cached, Requests[I], &Resp).ok());
      EXPECT_EQ(Resp.SerializeAsString(), Expected[I])
          << "проход " << Pass << ", запрос " << I;
    }
  }
}

// GetBars: результат с кэшем совпадает с результатом без кэша, повторный
// вызов не читает источник.
TEST(TimelineCache, GetBarsUsesCacheAndEqualsUncached) {
  core::testing::Fixture F = core::testing::fixture1();
  CountingBarSource CountingBars(F.Bars);
  pb::MarketSpec Market = fixtureMarket();
  Market.set_from_date("2024-01-11");
  Market.set_warmup_bars(2);

  pb::GetBarsRequest Request;
  *Request.mutable_market() = Market;
  Request.set_from_step(-2);
  Request.set_to_step(5);

  worker::Context Plain{CountingBars, F.Calendar, F.Ruonia};
  pb::GetBarsResponse Expected;
  ASSERT_TRUE(worker::handleGetBars(Plain, Request, &Expected).ok());
  ASSERT_GT(Expected.bars_size(), 0);

  worker::TimelineCache Cache(4);
  worker::Context Cached{CountingBars, F.Calendar, F.Ruonia, &Cache};
  pb::GetBarsResponse Miss;
  ASSERT_TRUE(worker::handleGetBars(Cached, Request, &Miss).ok());
  const std::int64_t LoadsAfterMiss = CountingBars.loadCount();
  EXPECT_EQ(Cache.size(), 1);
  pb::GetBarsResponse Hit;
  ASSERT_TRUE(worker::handleGetBars(Cached, Request, &Hit).ok());

  EXPECT_EQ(CountingBars.loadCount(), LoadsAfterMiss);
  EXPECT_EQ(Miss.SerializeAsString(), Expected.SerializeAsString());
  EXPECT_EQ(Hit.SerializeAsString(), Expected.SerializeAsString());
  EXPECT_EQ(Cache.size(), 1);
}

// Бенчмарк кэшируется одной записью на сессию, а не по записи на каждый день
// курсора: отчёты на разных шагах не вытесняют шкалу сессии.
TEST(TimelineCache, BenchmarkUsesSingleCacheEntry) {
  core::testing::Fixture F = core::testing::fixture1();
  addImoex(F);
  worker::TimelineCache Cache(8);
  worker::Context Context{F.Bars, F.Calendar, F.Ruonia, &Cache};
  pb::MarketSpec Market = fixtureMarket();
  Market.set_benchmark_ticker("IMOEX");

  for (const std::int64_t UpTo : {2, 3, 4, 5, 6, 7, 9}) {
    pb::ReplaySessionRequest R;
    *R.mutable_market() = Market;
    R.set_up_to_step(UpTo);
    R.set_events_from_step(-1);
    R.set_include_report(true);
    pb::ReplaySessionResponse Resp;
    ASSERT_TRUE(worker::handleReplaySession(Context, R, &Resp).ok());
    EXPECT_EQ(Cache.size(), 2) << "шаг " << UpTo; // шкала сессии + бенчмарк
  }
}

namespace {

constexpr std::int64_t kDayMs = 86'400'000;
constexpr std::int64_t kMoscowOffsetMs = 3 * 3'600'000;

// Дата (yyyymmdd) по числу дней от 1970-01-01 (алгоритм civil_from_days).
std::int32_t ymdFromDays(std::int64_t Days) {
  Days += 719468;
  const std::int64_t Era = (Days >= 0 ? Days : Days - 146096) / 146097;
  const std::int64_t Doe = Days - Era * 146097;
  const std::int64_t Yoe =
      (Doe - Doe / 1460 + Doe / 36524 - Doe / 146096) / 365;
  const std::int64_t Doy = Doe - (365 * Yoe + Yoe / 4 - Yoe / 100);
  const std::int64_t Mp = (5 * Doy + 2) / 153;
  const std::int64_t D = Doy - (153 * Mp + 2) / 5 + 1;
  const std::int64_t M = Mp < 10 ? Mp + 3 : Mp - 9;
  const std::int64_t Y = Yoe + Era * 400 + (M <= 2 ? 1 : 0);
  return static_cast<std::int32_t>(Y * 10000 + M * 100 + D);
}

// Перцентиль P по индексу ceil(P * N) - 1 в отсортированном ряду.
double percentile(const std::vector<double> &Sorted, double P) {
  const auto Index = static_cast<std::size_t>(
      std::ceil(P * static_cast<double>(Sorted.size())));
  return Sorted[Index - 1];
}

} // namespace

// Замер шага плеера по НФТ-7 (p95 <= 300 мс), близкий к худшему случаю:
// 2000 дневных шагов, 5 бумаг, журнал с сотнями заявок (рыночных и
// неисполняющихся лимитных), шаг в конце шкалы (replaySession идёт с шага 0),
// два варианта - без отчёта и с отчётом и бенчмарком. Источник баров в памяти:
// замер охватывает обработчик и кэш, но не чтение ClickHouse, не gRPC и не
// сеть. Вывод - ctest -R PlayerStepBenchmarkNft7 -V; порог 300 мс проверяется
// только при NDEBUG (PRESET=release).
TEST(TimelineCache, PlayerStepBenchmarkNft7) {
  constexpr std::int64_t kSteps = 2000;
  constexpr int kInstruments = 5;
  constexpr int kIterations = 100;
  const std::string Snapshot = "bench-long";
  const std::int64_t FirstDay = 14'610; // 2010-01-01

  core::testing::Fixture F;
  std::vector<core::TradingDay> Calendar;
  for (std::int64_t Day = 0; Day < kSteps; ++Day)
    Calendar.push_back(core::TradingDay{ymdFromDays(FirstDay + Day), true});
  F.Calendar.add(Snapshot, Calendar);
  F.Ruonia.add(Snapshot, {core::DailyRate{ymdFromDays(FirstDay - 1),
                                          core::Decimal::parse("15.0")}});

  pb::MarketSpec Market;
  Market.set_snapshot_id(Snapshot);
  Market.set_timeframe(pb::D1);
  Market.set_from_date(worker::formatIsoDate(Calendar.front().Yyyymmdd));
  Market.set_to_date(worker::formatIsoDate(Calendar.back().Yyyymmdd));
  *Market.mutable_costs() = fixtureMarket().costs();
  *Market.mutable_initial_cash() = dec("1000000000");

  std::vector<std::string> Tickers;
  for (int I = 0; I < kInstruments; ++I)
    Tickers.push_back("SYN" + std::to_string(I));
  Tickers.push_back("IMOEX");
  for (std::size_t K = 0; K < Tickers.size(); ++K) {
    F.Bars.addSecurity(Snapshot, core::Security{Tickers[K], "", 1,
                                                core::Decimal::parse("0.01")});
    std::vector<core::Bar> Bars;
    for (std::int64_t Day = 0; Day < kSteps; ++Day) {
      const std::int64_t Open = (FirstDay + Day) * kDayMs - kMoscowOffsetMs;
      const std::int64_t Price =
          100 + (Day * 7 + 13 * static_cast<std::int64_t>(K)) % 50;
      core::Bar B;
      B.TsOpenMs = Open;
      B.AvailableAtMs = Open + kDayMs;
      B.Open = core::Decimal::fromUnits(Price);
      B.Close = core::Decimal::fromUnits(Price + 1);
      B.High = core::Decimal::fromUnits(Price + 3);
      B.Low = core::Decimal::fromUnits(Price - 1);
      B.Volume = 1'000'000;
      B.Value = B.Close.times(B.Volume);
      Bars.push_back(B);
    }
    F.Bars.addSeries(Snapshot, Tickers[K], 86400, std::move(Bars));
  }
  for (int I = 0; I < kInstruments; ++I) {
    pb::Instrument *Inst = Market.add_instruments();
    Inst->set_key("K" + std::to_string(I));
    Inst->set_ticker(Tickers[static_cast<std::size_t>(I)]);
    Inst->set_lot_size(1);
    *Inst->mutable_price_step() = dec("0.01");
  }
  Market.set_benchmark_ticker("IMOEX");

  pb::ReplaySessionRequest Request;
  *Request.mutable_market() = Market;
  std::int64_t Seq = 0;
  for (std::int64_t Step = 0; Step < kSteps - 1; Step += 4) {
    const std::string Key = "K" + std::to_string((Step / 4) % kInstruments);
    // Рыночная покупка, редкая продажа меньшего объёма и лимитная покупка
    // далеко от рынка (остаётся активной до конца шкалы).
    *Request.add_journal() = marketOrder(Seq++, Step, Key.c_str(), pb::BUY, 2);
    if (Step % 40 == 0)
      *Request.add_journal() =
          marketOrder(Seq++, Step, Key.c_str(), pb::SELL, 1);
    if (Step % 20 == 0)
      *Request.add_journal() =
          limitOrder(Seq++, Step, Key.c_str(), pb::BUY, 1, "1.00");
  }
  Request.set_up_to_step(kSteps - 1);
  Request.set_events_from_step(kSteps - 2);
  Request.set_include_history(false);

  for (const bool Report : {false, true}) {
    worker::TimelineCache Cache(4);
    worker::Context Context{F.Bars, F.Calendar, F.Ruonia, &Cache};
    Request.set_include_report(Report);

    // Первый вызов - промах (загрузка шкалы), в ряд замера не входит.
    pb::ReplaySessionResponse First;
    const auto ColdStart = std::chrono::steady_clock::now();
    const auto FirstStatus =
        worker::handleReplaySession(Context, Request, &First);
    ASSERT_TRUE(FirstStatus.ok()) << FirstStatus.error_message();
    const double ColdMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - ColdStart)
                              .count();
    ASSERT_EQ(First.total_steps(), kSteps);

    std::vector<double> LatenciesMs;
    for (int I = 0; I < kIterations; ++I) {
      pb::ReplaySessionResponse Resp;
      const auto Start = std::chrono::steady_clock::now();
      const auto Status = worker::handleReplaySession(Context, Request, &Resp);
      LatenciesMs.push_back(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - Start)
                                .count());
      ASSERT_TRUE(Status.ok());
    }
    std::sort(LatenciesMs.begin(), LatenciesMs.end());
    const double P95 = percentile(LatenciesMs, 0.95);

    std::cout << "[BENCHMARK NFT-7] replaySession с кэшем шкалы: " << kSteps
              << " шагов D1, " << kInstruments << " бумаг, журнал "
              << Request.journal_size() << " записей, активных заявок "
              << First.orders_size() << ", шаг " << kSteps - 1
              << " (replay с шага 0), отчёт с бенчмарком "
              << (Report ? "да" : "нет") << ", источник баров в памяти, "
              << kIterations << " итераций, холодный вызов " << ColdMs
              << " мс\n  p50: " << percentile(LatenciesMs, 0.50)
              << " мс, p90: " << percentile(LatenciesMs, 0.90)
              << " мс, p95: " << P95
              << " мс, p99: " << percentile(LatenciesMs, 0.99)
              << " мс, max: " << LatenciesMs.back() << " мс" << std::endl;

    // Порог НФТ-7 проверяется только в оптимизированной сборке: в Debug
    // (-O0) переигрывание журнала на порядок медленнее и порог не
    // показателен.
#ifdef NDEBUG
    EXPECT_LE(P95, 300.0);
#endif
  }
}
