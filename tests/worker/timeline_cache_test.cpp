#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

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

// Различные ключи дают разные записи в кэше.
TEST(TimelineCache, DifferentKeysAreDistinguished) {
  worker::TimelineCache Cache(10);
  const auto Ptr = makeSampleTimeline();

  const auto Key1 = sampleKey("snap-1", 20240109, 20240122);
  const auto Key2 = sampleKey("snap-2", 20240109, 20240122);
  const auto Key3 = sampleKey("snap-1", 20240110, 20240122);

  Cache.put(Key1, Ptr);
  EXPECT_EQ(Cache.size(), 1);
  EXPECT_NE(Cache.get(Key1), nullptr);
  EXPECT_EQ(Cache.get(Key2), nullptr);
  EXPECT_EQ(Cache.get(Key3), nullptr);

  Cache.put(Key2, Ptr);
  EXPECT_EQ(Cache.size(), 2);
  EXPECT_NE(Cache.get(Key2), nullptr);
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

// Замер производительности шага плеера по НФТ-7 (p95 <= 300 мс на фикстуре).
TEST(TimelineCache, PlayerStepBenchmarkNft7) {
  core::testing::Fixture F = core::testing::fixture1();
  worker::TimelineCache Cache(4);
  worker::Context Context{F.Bars, F.Calendar, F.Ruonia, &Cache};

  const pb::MarketSpec Market = fixtureMarket();
  pb::ReplaySessionRequest Request;
  *Request.mutable_market() = Market;

  // Разогрев (первый вызов загружает шкалу в кэш)
  Request.set_up_to_step(0);
  pb::ReplaySessionResponse WarmupResp;
  ASSERT_TRUE(worker::handleReplaySession(Context, Request, &WarmupResp).ok());
  ASSERT_EQ(Cache.size(), 1);

  // Имитируем 100 последовательных шагов плеера (воспроизведение сессии)
  constexpr int kIterations = 100;
  std::vector<double> LatenciesMs;
  LatenciesMs.reserve(kIterations);

  for (int Step = 0; Step < kIterations; ++Step) {
    const std::int64_t CurrentStep = Step % 10;
    Request.set_up_to_step(CurrentStep);
    Request.set_events_from_step(CurrentStep > 0 ? CurrentStep - 1 : -1);
    Request.set_include_history(false);
    Request.set_include_report(false);

    const auto Start = std::chrono::high_resolution_clock::now();
    pb::ReplaySessionResponse Resp;
    const auto Status = worker::handleReplaySession(Context, Request, &Resp);
    const auto Elapsed = std::chrono::high_resolution_clock::now() - Start;

    ASSERT_TRUE(Status.ok());
    const double Ms =
        std::chrono::duration<double, std::milli>(Elapsed).count();
    LatenciesMs.push_back(Ms);
  }

  std::sort(LatenciesMs.begin(), LatenciesMs.end());
  const double P50 = LatenciesMs[kIterations * 50 / 100];
  const double P90 = LatenciesMs[kIterations * 90 / 100];
  const double P95 = LatenciesMs[kIterations * 95 / 100];
  const double P99 = LatenciesMs[kIterations * 99 / 100];
  const double Max = LatenciesMs.back();

  std::cout << "[BENCHMARK NFT-7] Шаг плеера с TimelineCache (100 итераций):"
            << "\n  p50: " << P50 << " ms"
            << "\n  p90: " << P90 << " ms"
            << "\n  p95: " << P95 << " ms"
            << "\n  p99: " << P99 << " ms"
            << "\n  max: " << Max << " ms" << std::endl;

  // НФТ-7 требует p95 <= 300 мс. С кэшем время должно быть значительно меньше!
  EXPECT_LE(P95, 300.0);
  // Проверяем, что шаг плеера укладывается в единицы миллисекунд (на самом деле
  // < 1 мс).
  EXPECT_LE(P95, 10.0);
}
