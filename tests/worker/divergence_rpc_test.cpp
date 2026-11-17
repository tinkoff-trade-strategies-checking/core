#include <gtest/gtest.h>

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "clickhouse/clickhouse_client.hpp"
#include "core/market/bar_source.hpp"
#include "core/money/proto_conv.hpp"
#include "worker_test_support.hpp"

namespace {

using namespace worker_test;

// Источник, который бросает заданное исключение при любом чтении.
class ThrowingBarSource final : public core::BarSource {
public:
  explicit ThrowingBarSource(std::function<void()> Action)
      : Thrower(std::move(Action)) {}
  std::vector<core::Bar> load(const core::BarRequest &) override {
    Thrower();
    return {};
  }
  core::Security security(const std::string &, const std::string &) override {
    Thrower();
    return {};
  }

private:
  std::function<void()> Thrower;
};

// fixture-1, только бумага A (лот 10): close 100, 101, 103, 106, 104, 101, 99,
// 102, 105, 108 - шаги 0…9.
pb::MarketSpec marketA() {
  pb::MarketSpec Market = fixtureMarket();
  Market.mutable_instruments()->RemoveLast();
  return Market;
}

constexpr const char *kSilent =
    "rule \"r\" when false then buy 1 lots because \"никогда\"\n";
constexpr const char *kBuyAt3 =
    "rule \"r\" when close == 106 then buy 5 lots because \"цена 106\"\n";

core::Decimal fromProto(const pb::Decimal &D) {
  return core::Decimal::fromUnitsNano(D.units(), D.nano());
}

class DivergenceRpc : public ::testing::Test {
protected:
  void addAlgorithm(const std::string &Id, const std::string &Source) {
    pb::AlgorithmRef *Ref = Request.add_algorithms();
    Ref->set_id(Id);
    Ref->mutable_strategy()->set_source(Source);
  }

  grpc::Status analyze(pb::DivergenceResponse *Response) {
    worker::Context Context{Fixture.Bars, Fixture.Calendar};
    *Request.mutable_market() = Market;
    return worker::handleAnalyzeDivergences(Context, Request, Response);
  }

  core::testing::Fixture Fixture = core::testing::fixture1();
  pb::MarketSpec Market = marketA();
  pb::DivergenceRequest Request;
  DivergenceRpc() { Request.set_last_step(9); }
};

} // namespace

// Алгоритм покупает на шаге 3, человек бездействует: «упущенное действие»; цена
// ошибки - прибыль независимого портфеля.
TEST_F(DivergenceRpc, MissedAction) {
  addAlgorithm("ref", kBuyAt3);
  pb::DivergenceResponse Response;
  ASSERT_TRUE(analyze(&Response).ok());

  ASSERT_EQ(Response.divergences_size(), 1);
  const pb::Divergence &D = Response.divergences(0);
  EXPECT_EQ(D.algorithm_id(), "ref");
  EXPECT_EQ(D.instrument_key(), "A");
  EXPECT_EQ(D.type(), pb::MISSED);
  EXPECT_EQ(D.human_step(), -1);
  EXPECT_EQ(D.algo_step(), 3);
  EXPECT_EQ(D.human_lots(), 0);
  EXPECT_EQ(D.algo_lots(), 5);
  EXPECT_EQ(D.horizon_step(), 9);
  EXPECT_EQ(D.reason(), "цена 106");

  // Человек в этом расхождении не действовал, портфель человека пуст, поэтому V
  // алгоритма совпадает с прибылью независимого прогона с теми же заявками (то
  // же исполнение, комиссия и сбор).
  ASSERT_EQ(Response.algorithm_portfolios_size(), 1);
  const core::Decimal Profit =
      fromProto(Response.algorithm_portfolios(0).report().final_equity()) -
      core::Decimal::fromUnits(1000000);
  EXPECT_EQ(fromProto(D.cost()), Profit.round(2, core::RoundingMode::HalfUp));
  EXPECT_NEAR(D.cost_percent(),
              fromProto(D.cost()).toDouble() / 1000000.0 * 100.0, 1e-9);
  EXPECT_EQ(Response.algorithm_portfolios(0).total_steps(), 10);

  ASSERT_EQ(Response.consensus_size(), 1);
  EXPECT_FALSE(Response.consensus(0).human_action());
  EXPECT_EQ(Response.consensus(0).agreeing(), 1);
  EXPECT_EQ(Response.consensus(0).total(), 1);
  EXPECT_EQ(Response.consensus(0).side(), pb::BUY);
  EXPECT_EQ(Response.missed_profitable_size(), 0);
}

// Человек купил и продал, алгоритм молчит: два «лишних действия» и пропущенная
// прибыльная сделка (ФТ-20).
TEST_F(DivergenceRpc, ExcessAndMissedProfitableTrade) {
  addAlgorithm("ref", kSilent);
  *Request.add_journal() =
      marketOrder(0, 5, "A", pb::BUY, 5); // исполнится по open шага 6 = 101
  *Request.add_journal() =
      marketOrder(1, 7, "A", pb::SELL, 5); // исполнится по open шага 8 = 102
  pb::DivergenceResponse Response;
  ASSERT_TRUE(analyze(&Response).ok());

  ASSERT_EQ(Response.divergences_size(), 2);
  EXPECT_EQ(Response.divergences(0).type(), pb::EXCESS);
  EXPECT_EQ(Response.divergences(0).human_step(), 5);
  EXPECT_EQ(Response.divergences(0).algo_step(), -1);
  EXPECT_EQ(Response.divergences(0).human_lots(), 5);
  EXPECT_EQ(Response.divergences(0).horizon_step(), 7);
  EXPECT_EQ(Response.divergences(1).human_lots(), -5);
  EXPECT_EQ(Response.divergences(1).horizon_step(), 9);
  EXPECT_EQ(Response.divergences(1).reason(),
            "алгоритм здесь не действовал бы");

  ASSERT_EQ(Response.missed_profitable_size(), 1);
  const pb::MissedProfitableTrade &Trade = Response.missed_profitable(0);
  EXPECT_EQ(Trade.instrument_key(), "A");
  EXPECT_EQ(Trade.entry_step(), 6);
  EXPECT_EQ(Trade.exit_step(), 8);
  EXPECT_EQ(Trade.lots(), 5);
  EXPECT_GT(fromProto(Trade.net_pnl()).sign(), 0);
  ASSERT_EQ(Trade.algorithm_ids_size(), 1);
  EXPECT_EQ(Trade.algorithm_ids(0), "ref");
  // Цены пары - цены сделок человека (по одной на вход и выход), без издержек
  // сверх спреда и проскальзывания.
  worker::Context Context{Fixture.Bars, Fixture.Calendar};
  pb::ReplaySessionRequest Replay;
  *Replay.mutable_market() = Market;
  *Replay.mutable_journal() = Request.journal();
  Replay.set_up_to_step(9);
  Replay.set_events_from_step(-1);
  pb::ReplaySessionResponse Human;
  ASSERT_TRUE(worker::handleReplaySession(Context, Replay, &Human).ok());
  ASSERT_EQ(Human.fills_size(), 2);
  EXPECT_EQ(Trade.entry_price().SerializeAsString(),
            Human.fills(0).price().SerializeAsString());
  EXPECT_EQ(Trade.exit_price().SerializeAsString(),
            Human.fills(1).price().SerializeAsString());
  EXPECT_GT(fromProto(Trade.entry_price()),
            core::Decimal::parse("101")); // open 101 + спред и проскальзывание

  // Независимый портфель молчащего алгоритма: ни одной сделки.
  ASSERT_EQ(Response.algorithm_portfolios_size(), 1);
  EXPECT_EQ(Response.algorithm_portfolios(0).fills_size(), 0);
}

TEST_F(DivergenceRpc, SameActionHasNoDivergence) {
  addAlgorithm("ref", kBuyAt3);
  *Request.add_journal() = marketOrder(0, 3, "A", pb::BUY, 5);
  pb::DivergenceResponse Response;
  ASSERT_TRUE(analyze(&Response).ok());
  EXPECT_EQ(Response.divergences_size(), 0);
  EXPECT_EQ(Response.consensus_size(), 0);
}

TEST_F(DivergenceRpc, BadRequestsAreInvalidArgument) {
  pb::DivergenceResponse Response;
  // lastStep вне шкалы.
  addAlgorithm("ref", kSilent);
  Request.set_last_step(10);
  EXPECT_EQ(analyze(&Response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  Request.set_last_step(9);

  // Исходник не разбирается.
  pb::DivergenceRequest Broken = Request;
  Broken.mutable_algorithms(0)->mutable_strategy()->set_source(
      "rule без смысла");
  Request = Broken;
  EXPECT_EQ(analyze(&Response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  // Больше пяти алгоритмов.
  Request.clear_algorithms();
  for (int I = 0; I < 6; ++I)
    addAlgorithm("a" + std::to_string(I), kSilent);
  EXPECT_EQ(analyze(&Response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  // Неизвестная бумага в журнале.
  Request.clear_algorithms();
  addAlgorithm("ref", kSilent);
  *Request.add_journal() = marketOrder(0, 2, "ZZZ", pb::BUY, 5);
  EXPECT_EQ(analyze(&Response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(Response.divergences_size(), 0);
}

TEST_F(DivergenceRpc, UnknownSnapshotIsFailedPrecondition) {
  addAlgorithm("ref", kSilent);
  Market.set_snapshot_id("нет-такого");
  pb::DivergenceResponse Response;
  EXPECT_EQ(analyze(&Response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
}

// Ни одно исключение не выходит из обработчика.
TEST(DivergenceRpcExceptions, MappedToStatus) {
  core::InMemoryCalendarSource Calendar;
  const auto StatusOf = [&](std::function<void()> Thrower) {
    ThrowingBarSource Bars(std::move(Thrower));
    worker::Context Context{Bars, Calendar};
    pb::DivergenceRequest Request;
    *Request.mutable_market() = marketA();
    Request.set_last_step(9);
    pb::DivergenceResponse Response;
    return worker::handleAnalyzeDivergences(Context, Request, &Response);
  };

  EXPECT_EQ(StatusOf([] {
              throw worker::ClickHouseError("секрет: пароль db");
            }).error_code(),
            grpc::StatusCode::UNAVAILABLE);
  EXPECT_EQ(StatusOf([] { throw core::SnapshotNotFound("нет"); }).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(StatusOf([] { throw std::invalid_argument("плохо"); }).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  const grpc::Status Internal = StatusOf(
      [] { throw std::runtime_error("внутренняя деталь /etc/passwd"); });
  EXPECT_EQ(Internal.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(Internal.error_message().find("passwd"), std::string::npos);
  EXPECT_EQ(StatusOf([] { throw 42; }).error_code(),
            grpc::StatusCode::INTERNAL);
}
