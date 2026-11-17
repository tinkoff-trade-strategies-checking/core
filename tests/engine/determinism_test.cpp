// Детерминизм (INV-4): один и тот же вход и журнал дают побитово то же
// состояние сессии.
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "core/engine/digest.hpp"
#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "support/fixture1.hpp"

namespace core {
namespace {

std::shared_ptr<const Timeline> fixtureTimeline() {
  auto F = testing::fixture1();
  return std::make_shared<const Timeline>(
      Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                     Timeframe::D1, 20240109, 20240122));
}

SessionSpec fixtureSpec() {
  SessionSpec Spec;
  Spec.Instruments = testing::fixture1().Instruments;
  Spec.Costs = CostConfig::defaults();
  Spec.InitialCash = Decimal::parse("1000000");
  return Spec;
}

JournalEntry market(std::int64_t Seq, std::int64_t Step, Side OrderSide,
                    std::int64_t Lots) {
  OrderSpec Spec;
  Spec.Instrument = 0;
  Spec.OrderSide = OrderSide;
  Spec.Type = OrderType::Market;
  Spec.Lots = Lots;
  JournalEntry E;
  E.Seq = Seq;
  E.Step = Step;
  E.Command = PlaceCmd{Spec, std::nullopt};
  return E;
}

// Покупки и продажи на разных шагах: в журнале есть сделки.
std::vector<JournalEntry> sampleJournal(std::int64_t Lots = 10) {
  return {market(0, 0, Side::Buy, Lots), market(1, 2, Side::Sell, 4),
          market(2, 4, Side::Buy, 3), market(3, 7, Side::Sell, Lots - 1)};
}

ReplayResult replay(const std::vector<JournalEntry> &Journal) {
  return replaySession(fixtureSpec(), fixtureTimeline(), Journal, 9, 0);
}

TEST(Determinism, SameDigestTwice) {
  const ReplayResult First = replay(sampleJournal());
  const ReplayResult Second = replay(sampleJournal());
  ASSERT_GT(First.Engine.fills().size(), 3u); // прогон содержательный
  EXPECT_EQ(digestOf(First.Engine), digestOf(Second.Engine));
  EXPECT_NE(digestOf(First.Engine), 0u);

  const ReplayResult Other = replay(sampleJournal(12));
  EXPECT_NE(digestOf(First.Engine), digestOf(Other.Engine));
}

TEST(Determinism, DigestCoversFillsEquityAndStatus) {
  const ReplayResult R = replay(sampleJournal());
  const std::uint64_t Base = digestOf(R.Engine);

  // Копия движка - снимок состояния; дальше двигаем её отдельно.
  SessionEngine Fork = R.Engine;
  ASSERT_EQ(Fork.status(), SessionStatus::FinishedEnd);
  EXPECT_EQ(digestOf(Fork), Base);

  // Сессия, остановленная раньше (другой набор шагов и статус), даёт другой
  // отпечаток.
  const ReplayResult Short = replaySession(
      fixtureSpec(), fixtureTimeline(),
      std::vector<JournalEntry>{market(0, 0, Side::Buy, 10)}, 5, 0);
  EXPECT_NE(digestOf(Short.Engine), Base);

  // Другие издержки меняют сделки и кривую стоимости.
  SessionSpec Costly = fixtureSpec();
  Costly.Costs.CommissionRate =
      Costly.Costs.CommissionRate + Decimal::parse("0.001");
  const ReplayResult Priced =
      replaySession(Costly, fixtureTimeline(), sampleJournal(), 9, 0);
  EXPECT_NE(digestOf(Priced.Engine), Base);
}

} // namespace
} // namespace core
