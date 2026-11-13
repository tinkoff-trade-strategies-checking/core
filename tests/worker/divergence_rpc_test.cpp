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
  explicit ThrowingBarSource(std::function<void()> thrower) : thrower_(std::move(thrower)) {}
  std::vector<core::Bar> load(const core::BarRequest&) override {
    thrower_();
    return {};
  }
  core::Security security(const std::string&, const std::string&) override {
    thrower_();
    return {};
  }

 private:
  std::function<void()> thrower_;
};

// fixture-1, только бумага A (лот 10): close 100, 101, 103, 106, 104, 101, 99, 102, 105, 108 - шаги 0…9.
pb::MarketSpec marketA() {
  pb::MarketSpec market = fixtureMarket();
  market.mutable_instruments()->RemoveLast();
  return market;
}

constexpr const char* kSilent = "rule \"r\" when false then buy 1 lots because \"никогда\"\n";
constexpr const char* kBuyAt3 = "rule \"r\" when close == 106 then buy 5 lots because \"цена 106\"\n";

core::Decimal fromProto(const pb::Decimal& d) { return core::Decimal::fromUnitsNano(d.units(), d.nano()); }

class DivergenceRpc : public ::testing::Test {
 protected:
  void addAlgorithm(const std::string& id, const std::string& source) {
    pb::AlgorithmRef* ref = request_.add_algorithms();
    ref->set_id(id);
    ref->mutable_strategy()->set_source(source);
  }

  grpc::Status analyze(pb::DivergenceResponse* response) {
    worker::Context context{fixture_.bars, fixture_.calendar};
    *request_.mutable_market() = market_;
    return worker::handleAnalyzeDivergences(context, request_, response);
  }

  core::testing::Fixture fixture_ = core::testing::fixture1();
  pb::MarketSpec market_ = marketA();
  pb::DivergenceRequest request_;
  DivergenceRpc() { request_.set_last_step(9); }
};

}  // namespace

// Алгоритм покупает на шаге 3, человек бездействует: «упущенное действие»; цена ошибки - прибыль независимого портфеля.
TEST_F(DivergenceRpc, MissedAction) {
  addAlgorithm("ref", kBuyAt3);
  pb::DivergenceResponse response;
  ASSERT_TRUE(analyze(&response).ok());

  ASSERT_EQ(response.divergences_size(), 1);
  const pb::Divergence& d = response.divergences(0);
  EXPECT_EQ(d.algorithm_id(), "ref");
  EXPECT_EQ(d.instrument_key(), "A");
  EXPECT_EQ(d.type(), pb::MISSED);
  EXPECT_EQ(d.human_step(), -1);
  EXPECT_EQ(d.algo_step(), 3);
  EXPECT_EQ(d.human_lots(), 0);
  EXPECT_EQ(d.algo_lots(), 5);
  EXPECT_EQ(d.horizon_step(), 9);
  EXPECT_EQ(d.reason(), "цена 106");

  // Человек в этом расхождении не действовал, портфель человека пуст, поэтому V алгоритма совпадает с прибылью
  // независимого прогона с теми же заявками (то же исполнение, комиссия и сбор).
  ASSERT_EQ(response.algorithm_portfolios_size(), 1);
  const core::Decimal profit = fromProto(response.algorithm_portfolios(0).report().final_equity()) - core::Decimal::fromUnits(1000000);
  EXPECT_EQ(fromProto(d.cost()), profit.round(2, core::RoundingMode::HALF_UP));
  EXPECT_NEAR(d.cost_percent(), fromProto(d.cost()).toDouble() / 1000000.0 * 100.0, 1e-9);
  EXPECT_EQ(response.algorithm_portfolios(0).total_steps(), 10);

  ASSERT_EQ(response.consensus_size(), 1);
  EXPECT_FALSE(response.consensus(0).human_action());
  EXPECT_EQ(response.consensus(0).agreeing(), 1);
  EXPECT_EQ(response.consensus(0).total(), 1);
  EXPECT_EQ(response.consensus(0).side(), pb::BUY);
  EXPECT_EQ(response.missed_profitable_size(), 0);
}

// Человек купил и продал, алгоритм молчит: два «лишних действия» и пропущенная прибыльная сделка (ФТ-20).
TEST_F(DivergenceRpc, ExcessAndMissedProfitableTrade) {
  addAlgorithm("ref", kSilent);
  *request_.add_journal() = marketOrder(0, 5, "A", pb::BUY, 5);   // исполнится по open шага 6 = 101
  *request_.add_journal() = marketOrder(1, 7, "A", pb::SELL, 5);  // исполнится по open шага 8 = 102
  pb::DivergenceResponse response;
  ASSERT_TRUE(analyze(&response).ok());

  ASSERT_EQ(response.divergences_size(), 2);
  EXPECT_EQ(response.divergences(0).type(), pb::EXCESS);
  EXPECT_EQ(response.divergences(0).human_step(), 5);
  EXPECT_EQ(response.divergences(0).algo_step(), -1);
  EXPECT_EQ(response.divergences(0).human_lots(), 5);
  EXPECT_EQ(response.divergences(0).horizon_step(), 7);
  EXPECT_EQ(response.divergences(1).human_lots(), -5);
  EXPECT_EQ(response.divergences(1).horizon_step(), 9);
  EXPECT_EQ(response.divergences(1).reason(), "алгоритм здесь не действовал бы");

  ASSERT_EQ(response.missed_profitable_size(), 1);
  const pb::MissedProfitableTrade& trade = response.missed_profitable(0);
  EXPECT_EQ(trade.instrument_key(), "A");
  EXPECT_EQ(trade.entry_step(), 6);
  EXPECT_EQ(trade.exit_step(), 8);
  EXPECT_EQ(trade.lots(), 5);
  EXPECT_GT(fromProto(trade.net_pnl()).sign(), 0);
  ASSERT_EQ(trade.algorithm_ids_size(), 1);
  EXPECT_EQ(trade.algorithm_ids(0), "ref");
  // Цены пары - цены сделок человека (по одной на вход и выход), без издержек сверх спреда и проскальзывания.
  worker::Context context{fixture_.bars, fixture_.calendar};
  pb::ReplaySessionRequest replay;
  *replay.mutable_market() = market_;
  *replay.mutable_journal() = request_.journal();
  replay.set_up_to_step(9);
  replay.set_events_from_step(-1);
  pb::ReplaySessionResponse human;
  ASSERT_TRUE(worker::handleReplaySession(context, replay, &human).ok());
  ASSERT_EQ(human.fills_size(), 2);
  EXPECT_EQ(trade.entry_price().SerializeAsString(), human.fills(0).price().SerializeAsString());
  EXPECT_EQ(trade.exit_price().SerializeAsString(), human.fills(1).price().SerializeAsString());
  EXPECT_GT(fromProto(trade.entry_price()), core::Decimal::parse("101"));  // open 101 + спред и проскальзывание

  // Независимый портфель молчащего алгоритма: ни одной сделки.
  ASSERT_EQ(response.algorithm_portfolios_size(), 1);
  EXPECT_EQ(response.algorithm_portfolios(0).fills_size(), 0);
}

TEST_F(DivergenceRpc, SameActionHasNoDivergence) {
  addAlgorithm("ref", kBuyAt3);
  *request_.add_journal() = marketOrder(0, 3, "A", pb::BUY, 5);
  pb::DivergenceResponse response;
  ASSERT_TRUE(analyze(&response).ok());
  EXPECT_EQ(response.divergences_size(), 0);
  EXPECT_EQ(response.consensus_size(), 0);
}

TEST_F(DivergenceRpc, BadRequestsAreInvalidArgument) {
  pb::DivergenceResponse response;
  // lastStep вне шкалы.
  addAlgorithm("ref", kSilent);
  request_.set_last_step(10);
  EXPECT_EQ(analyze(&response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  request_.set_last_step(9);

  // Исходник не разбирается.
  pb::DivergenceRequest broken = request_;
  broken.mutable_algorithms(0)->mutable_strategy()->set_source("rule без смысла");
  request_ = broken;
  EXPECT_EQ(analyze(&response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  // Больше пяти алгоритмов.
  request_.clear_algorithms();
  for (int i = 0; i < 6; ++i) addAlgorithm("a" + std::to_string(i), kSilent);
  EXPECT_EQ(analyze(&response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  // Неизвестная бумага в журнале.
  request_.clear_algorithms();
  addAlgorithm("ref", kSilent);
  *request_.add_journal() = marketOrder(0, 2, "ZZZ", pb::BUY, 5);
  EXPECT_EQ(analyze(&response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(response.divergences_size(), 0);
}

TEST_F(DivergenceRpc, UnknownSnapshotIsFailedPrecondition) {
  addAlgorithm("ref", kSilent);
  market_.set_snapshot_id("нет-такого");
  pb::DivergenceResponse response;
  EXPECT_EQ(analyze(&response).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

// Ни одно исключение не выходит из обработчика.
TEST(DivergenceRpcExceptions, MappedToStatus) {
  core::InMemoryCalendarSource calendar;
  const auto statusOf = [&](std::function<void()> thrower) {
    ThrowingBarSource bars(std::move(thrower));
    worker::Context context{bars, calendar};
    pb::DivergenceRequest request;
    *request.mutable_market() = marketA();
    request.set_last_step(9);
    pb::DivergenceResponse response;
    return worker::handleAnalyzeDivergences(context, request, &response);
  };

  EXPECT_EQ(statusOf([] { throw worker::ClickHouseError("секрет: пароль db"); }).error_code(),
            grpc::StatusCode::UNAVAILABLE);
  EXPECT_EQ(statusOf([] { throw core::SnapshotNotFound("нет"); }).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(statusOf([] { throw std::invalid_argument("плохо"); }).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  const grpc::Status internal = statusOf([] { throw std::runtime_error("внутренняя деталь /etc/passwd"); });
  EXPECT_EQ(internal.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(internal.error_message().find("passwd"), std::string::npos);
  EXPECT_EQ(statusOf([] { throw 42; }).error_code(), grpc::StatusCode::INTERNAL);
}
