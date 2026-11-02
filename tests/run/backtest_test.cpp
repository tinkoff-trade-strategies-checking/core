#include "core/run/backtest.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "run/run_test_support.hpp"

namespace core {
namespace {

using testing::dec;
using testing::loadProgram;

RunOutput run(const std::string& source, const SessionSpec& spec, const std::shared_ptr<const Timeline>& timeline,
              const dsl::ParamValues& overrides = {}, const Progress& progress = nullptr) {
  const auto loaded = loadProgram(source);
  return runBacktest(spec, timeline, loaded.program, loaded.params, overrides, dsl::Level::Ok, progress);
}

const Order* findOrder(const RunOutput& out, std::int64_t id) {
  for (const Order& o : out.orders) {
    if (o.id == id) return &o;
  }
  return nullptr;
}

std::vector<const Order*> ordersOf(const RunOutput& out, const std::string& rule) {
  std::vector<const Order*> found;
  for (const Order& o : out.orders) {
    if (o.ruleName == rule) found.push_back(&o);
  }
  return found;
}

constexpr const char* kSimpleRules =
    "rule \"b\" when close > close[1] and position == 0 then buy 10 lots because \"рост\"\n"
    "rule \"s\" when close < close[1] and position > 0 then sell all because \"падение\"\n";

TEST(Backtest, SimpleRulesOnFixture) {
  const auto [spec, timeline] = testing::fixtureA();
  const RunOutput out = run(kSimpleRules, spec, timeline);

  EXPECT_EQ(out.status, RunStatus::Completed);
  EXPECT_EQ(out.level, dsl::Level::Ok);
  EXPECT_EQ(out.lastStep, 9);
  ASSERT_EQ(out.fills.size(), 3u);

  EXPECT_EQ(out.fills[0].step, 2);
  EXPECT_EQ(out.fills[0].side, Side::Buy);
  EXPECT_EQ(out.fills[0].lots, 10);
  EXPECT_EQ(out.fills[0].price, dec("101.07"));  // base 101, raw 101.0606

  EXPECT_EQ(out.fills[1].step, 5);
  EXPECT_EQ(out.fills[1].side, Side::Sell);
  EXPECT_EQ(out.fills[1].price, dec("103.93"));  // raw 103.9376

  EXPECT_EQ(out.fills[2].step, 8);
  EXPECT_EQ(out.fills[2].side, Side::Buy);
  EXPECT_EQ(out.fills[2].price, dec("102.07"));  // raw 102.0612

  const char* because[] = {"рост", "падение", "рост"};
  for (std::size_t i = 0; i < 3; ++i) {
    const Order* order = findOrder(out, out.fills[i].orderId);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->because, because[i]);
  }
  EXPECT_EQ(out.equity.size(), 10u);
  EXPECT_EQ(out.positionValuesByStep.size(), 10u);
  EXPECT_EQ(out.finalEquity, out.equity.back().equity);
  EXPECT_EQ(out.roundTrips.size(), 1u);
  EXPECT_EQ(out.digest, digestOf(out));
}

TEST(Backtest, SameEngineAsHuman) {
  // Стратегия с заменой и отменой заявок: журнал стратегии проигрывается движком как журнал человека.
  const auto [spec, timeline] = testing::fixtureA();
  const RunOutput out = run(std::string(kSimpleRules) +
                                "rule \"lim\" when close < 103 then buy 1 lots limit close * 0.9 because \"лимит\"\n"
                                "rule \"c\" when close >= 106 then cancel because \"отмена\"\n",
                            spec, timeline);
  ASSERT_FALSE(out.journal.empty());

  const ReplayResult replay = replaySession(spec, timeline, out.journal, out.lastStep, 0);
  ASSERT_EQ(replay.engine.fills().size(), out.fills.size());
  for (std::size_t i = 0; i < out.fills.size(); ++i) {
    EXPECT_TRUE(testing::sameFill(replay.engine.fills()[i], out.fills[i])) << "сделка " << i;
  }
  ASSERT_EQ(replay.engine.equity().size(), out.equity.size());
  for (std::size_t i = 0; i < out.equity.size(); ++i) {
    EXPECT_EQ(replay.engine.equity()[i].equity, out.equity[i].equity) << "шаг " << i;
  }
  ASSERT_EQ(replay.engine.orders().size(), out.orders.size());
  for (std::size_t i = 0; i < out.orders.size(); ++i) {
    EXPECT_EQ(replay.engine.orders()[i].status, out.orders[i].status) << "заявка " << i;
    EXPECT_EQ(replay.engine.orders()[i].reason, out.orders[i].reason) << "заявка " << i;
    EXPECT_EQ(replay.engine.orders()[i].closedStep, out.orders[i].closedStep) << "заявка " << i;
  }
}

TEST(Backtest, StopCancelledWhenFlat) {
  const auto [spec, timeline] = testing::fixtureA();
  const RunOutput out = run(
      "rule \"вход\" when close > close[1] and position == 0 then buy 10 lots because \"рост\"\n"
      "rule \"стоп\" when position > 0 then sell all stop entry_price * 0.5 because \"защитный стоп\"\n"
      "rule \"перекупленность\" when position > 0 and close > 105 then sell all because \"перекупленность\"\n",
      spec, timeline);

  // Выход по перекупленности: решение на шаге 3 (close 106), продажа на шаге 4; стоп снят с PositionClosed.
  const auto stops = ordersOf(out, "стоп");
  ASSERT_GE(stops.size(), 1u);
  EXPECT_EQ(stops[0]->status, OrderStatus::Cancelled);
  EXPECT_EQ(stops[0]->reason, Reason::PositionClosed);
  EXPECT_EQ(stops[0]->closedStep, 4);
  ASSERT_GE(out.fills.size(), 2u);
  EXPECT_EQ(out.fills[1].side, Side::Sell);
  EXPECT_EQ(out.fills[1].step, 4);
  // В журнале это запись CancelCmd с причиной PositionClosed.
  bool journaled = false;
  for (const JournalEntry& e : out.journal) {
    if (const auto* cancel = std::get_if<CancelCmd>(&e.command)) {
      journaled |= cancel->orderId == stops[0]->id && cancel->reason == Reason::PositionClosed && e.step == 4;
    }
  }
  EXPECT_TRUE(journaled);
}

TEST(Backtest, OneActiveOrderPerRule) {
  const auto [spec, timeline] = testing::fixtureA();
  // «moving»: цена лимита меняется каждый бар - прежняя заявка снимается. «fixed»: заявка не меняется - не дублируется.
  const RunOutput out = run(
      "rule \"moving\" when true then buy 1 lots limit close * 0.5 because \"подвижная\"\n"
      "rule \"fixed\" when true then buy 1 lots limit 50 because \"постоянная\"\n",
      spec, timeline);

  const auto fixedOrders = ordersOf(out, "fixed");
  ASSERT_EQ(fixedOrders.size(), 1u);
  EXPECT_EQ(fixedOrders[0]->placedStep, 0);

  const auto moving = ordersOf(out, "moving");
  ASSERT_EQ(moving.size(), 9u);  // шаги 0…8; на последнем шаге 9 сессия уже закончена
  for (std::size_t i = 0; i < moving.size(); ++i) {
    EXPECT_EQ(moving[i]->status, OrderStatus::Cancelled);
    EXPECT_EQ(moving[i]->reason, i + 1 < moving.size() ? Reason::RuleReplaced : Reason::SessionEnd) << "заявка " << i;
  }
  // На любом шаге у правила не больше одной активной заявки.
  for (std::int64_t step = 0; step <= out.lastStep; ++step) {
    int active = 0;
    for (const Order* o : moving) active += o->placedStep <= step && (o->closedStep < 0 || o->closedStep > step);
    EXPECT_LE(active, 1) << "шаг " << step;
  }
}

TEST(Backtest, CancelActionCancelsRestingOrders) {
  const auto [spec, timeline] = testing::fixtureA();
  const RunOutput out = run(
      "rule \"lim\" when close < 103 then buy 1 lots limit 50 because \"лимит\"\n"
      "rule \"c\" when close >= 103 then cancel because \"отмена\"\n",
      spec, timeline);
  const auto lim = ordersOf(out, "lim");
  ASSERT_GE(lim.size(), 1u);
  EXPECT_EQ(lim[0]->status, OrderStatus::Cancelled);
  EXPECT_EQ(lim[0]->reason, Reason::CancelledByUser);
  EXPECT_EQ(lim[0]->closedStep, 2);
}

TEST(Backtest, EachInstrumentGetsOwnOrder) {
  auto f = testing::fixture1();
  const auto timeline = std::make_shared<const Timeline>(
      Timeline::load(f.bars, f.calendar, f.snapshotId, f.instruments, Timeframe::D1, 20240109, 20240122));
  const RunOutput out = run("rule \"b\" when position == 0 then buy 1 lots because \"вход\"\n",
                            testing::specFor(f.instruments), timeline);
  bool a = false, b = false;
  for (const Fill& fill : out.fills) {
    a |= fill.instrument == 0;
    b |= fill.instrument == 1;
  }
  EXPECT_TRUE(a);
  EXPECT_TRUE(b);
  EXPECT_EQ(out.fills.size(), 2u);
}

TEST(Backtest, ProgressEveryThousandStepsAndAtEnd) {
  const auto [spec, timeline] = testing::syntheticTimeline(testing::randomWalk(2500));
  std::vector<std::pair<std::int64_t, std::int64_t>> calls;
  run(kSimpleRules, spec, timeline, {},
      [&](std::int64_t done, std::int64_t total) { calls.emplace_back(done, total); });
  const std::vector<std::pair<std::int64_t, std::int64_t>> expected{{1000, 2500}, {2000, 2500}, {2500, 2500}};
  EXPECT_EQ(calls, expected);
}

TEST(Backtest, RefusesBadOverride) {
  // INV-1: экземпляр стратегии заново проверяет программу; значение вне диапазона - отказ, не прогон.
  const auto [spec, timeline] = testing::fixtureA();
  const std::string source =
      "param n = 2 range 1..5 step 1\n"
      "rule \"b\" when close > sma(close, n) then buy 1 lots because \"выше средней\"\n";
  EXPECT_THROW(run(source, spec, timeline, {{"n", 0}}), std::invalid_argument);
  EXPECT_THROW(run(source, spec, timeline, {{"n", 100000}}), std::invalid_argument);
}

TEST(Backtest, StaticLevelCarriesIntoRunLevel) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto loaded = loadProgram(kSimpleRules);
  const RunOutput out = runBacktest(spec, timeline, loaded.program, loaded.params, {}, dsl::Level::Warning, nullptr);
  EXPECT_EQ(out.level, dsl::Level::Warning);
  EXPECT_TRUE(out.findings.empty());
}

TEST(Backtest, CountsRejections) {
  // Покупка на сумму больше капитала не проходит проверку денег при подаче: отказ попадает в счётчик.
  const auto [spec, timeline] = testing::fixtureA();
  const RunOutput out = run("rule \"b\" when true then buy 99999999 lots because \"много\"\n", spec, timeline);
  EXPECT_GT(out.rejections, 0);
  EXPECT_EQ(out.fills.size(), 0u);
  EXPECT_EQ(out.orders.front().status, OrderStatus::Rejected);
}

// Правила «угадывателя»: купить на низком close (< 100), продать на высоком (> 100).
constexpr const char* kFlipRules =
    "rule \"b\" when close < 100 and position == 0 then buy 1 lots because \"низко\"\n"
    "rule \"s\" when close > 100 and position > 0 then sell all because \"высоко\"\n";

const Finding* finding(const RunOutput& out, const std::string& code) {
  for (const Finding& f : out.findings) {
    if (f.code == code) return &f;
  }
  return nullptr;
}

// Бары с гэпами: нечётные бары открываются с гэпом вверх (+4 % к close предыдущего), чётные - с гэпом вниз.
std::pair<SessionSpec, std::shared_ptr<const Timeline>> gappyTimeline(std::size_t bars, bool upThenDown) {
  std::vector<std::string> closes, opens;
  for (std::size_t i = 0; i < bars; ++i) {
    const bool low = (i % 2 == 0) == upThenDown;  // «низкий» бар: close 99, «высокий»: close 104
    closes.push_back(low ? "99" : "104");
    opens.push_back(low ? "99" : "103");
  }
  return testing::syntheticTimeline(closes, opens);
}

TEST(RuntimeChecks, GapLuckWarning) {
  // Шесть гэпов вверх: покупка за бар до каждого (и продажи перед гэпами вниз между ними).
  const auto [spec, timeline] = gappyTimeline(14, true);
  const RunOutput out = run(kFlipRules, spec, timeline);
  const Finding* f = finding(out, "W-RT01");
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->level, dsl::Level::Warning);
  EXPECT_EQ(out.level, dsl::Level::Warning);
  EXPECT_EQ(out.status, RunStatus::Completed);  // предупреждение прогон не останавливает
}

TEST(RuntimeChecks, GapsNotGuessedGiveNoWarning) {
  // Тот же рисунок цен, но стратегия торгует против гэпов: покупка перед гэпом вниз, продажа перед гэпом вверх.
  const auto [spec, timeline] = gappyTimeline(14, false);
  const RunOutput out = run(
      "rule \"b\" when close > 100 and position == 0 then buy 1 lots because \"высоко\"\n"
      "rule \"s\" when close < 100 and position > 0 then sell all because \"низко\"\n",
      spec, timeline);
  EXPECT_GE(out.fills.size(), 5u);
  EXPECT_EQ(finding(out, "W-RT01"), nullptr);
  EXPECT_EQ(out.level, dsl::Level::Ok);
}

TEST(RuntimeChecks, TooEvenResultWarning) {
  // Растущий рынок (+2 % за бар), сделка на каждом баре: все закрытые пары прибыльные.
  std::vector<std::string> closes;
  double price = 100;
  for (int i = 0; i < 90; ++i) {
    char text[32];
    std::snprintf(text, sizeof text, "%.2f", price);
    closes.emplace_back(text);
    price *= 1.02;
  }
  const auto [spec, timeline] = testing::syntheticTimeline(closes);
  const std::string rules =
      "rule \"b\" when position == 0 then buy 1 lots because \"вход\"\n"
      "rule \"s\" when position > 0 then sell all because \"выход\"\n";
  const RunOutput out = run(rules, spec, timeline);
  ASSERT_GE(out.roundTrips.size(), 30u);
  const Finding* f = finding(out, "W-RT02");
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->level, dsl::Level::Warning);

  // Меньше 30 закрытых пар - предупреждения нет.
  const auto [shortSpec, shortTimeline] =
      testing::syntheticTimeline(std::vector<std::string>(closes.begin(), closes.begin() + 30));
  const RunOutput shortRun = run(rules, shortSpec, shortTimeline);
  EXPECT_LT(shortRun.roundTrips.size(), 30u);
  EXPECT_EQ(finding(shortRun, "W-RT02"), nullptr);
}

// Гэпы на границах порогов: total гэпов вверх (close 100 -> open 103), перед guessed из них - покупка, перед остальными -
// продажа. Фактически заявки и сделки собраны вручную: проверяется сама проверка, не стратегия.
std::vector<Finding> gapFindings(int guessed, int total) {
  std::vector<std::string> closes, opens;
  for (int j = 0; j < total; ++j) {
    closes.insert(closes.end(), {"100", "103"});
    opens.insert(opens.end(), {j == 0 ? "100" : "103", "103"});
  }
  const auto [spec, timeline] = testing::syntheticTimeline(closes, opens);
  std::vector<Order> orders;
  std::vector<Fill> fills;
  for (int j = 0; j < total; ++j) {
    const Side side = j < guessed ? Side::Buy : Side::Sell;
    Order order;
    order.id = j * 2;
    order.spec.side = side;
    order.spec.lots = 1;
    order.placedStep = 2 * j;
    orders.push_back(order);
    Fill fill;
    fill.orderId = order.id;
    fill.step = 2 * j + 1;
    fill.side = side;
    fill.lots = 1;
    fill.basePrice = fill.price = dec("103");
    fills.push_back(fill);
  }
  return detail::runtimeFindings(*timeline, orders, fills, {});
}

TEST(RuntimeChecks, GapThresholdsAreInclusiveOnShareAndCount) {
  EXPECT_TRUE(gapFindings(4, 4).empty());     // 100 %, но угаданных меньше 5
  EXPECT_EQ(gapFindings(5, 5).size(), 1u);    // ровно 5
  EXPECT_TRUE(gapFindings(15, 19).empty());   // 78,9 % < 80 %
  EXPECT_EQ(gapFindings(16, 20).size(), 1u);  // ровно 80 %
}

std::vector<Finding> evenFindings(int trips, int wins) {
  const auto [spec, timeline] = testing::fixtureA();
  std::vector<RoundTrip> roundTrips;
  for (int i = 0; i < trips; ++i) {
    RoundTrip trip{};
    trip.exitStep = i;
    trip.netPnl = i < wins ? dec("10") : dec("-10");
    roundTrips.push_back(trip);
  }
  return detail::runtimeFindings(*timeline, {}, {}, roundTrips);
}

TEST(RuntimeChecks, TooEvenThresholds) {
  EXPECT_TRUE(evenFindings(29, 29).empty());    // меньше 30 пар
  EXPECT_TRUE(evenFindings(30, 27).empty());    // ровно 90 % - строго больше нужно
  EXPECT_EQ(evenFindings(30, 28).size(), 1u);   // 93 %
  EXPECT_TRUE(evenFindings(30, 29).size() == 1u);
}

TEST(Backtest, RejectedReplacementKeepsOldOrder) {
  // Стоп правила «стоп» с каждым баром отодвигается вверх и на третьем баре позиции оказывается выше close - новая
  // заявка отклонена, а прежняя остаётся: у правила не пропадает защита.
  const auto [spec, timeline] = testing::syntheticTimeline(std::vector<std::string>(9, "100"));
  const RunOutput out = run(
      "rule \"in\" when position == 0 then buy 1 lots because \"вход\"\n"
      "rule \"стоп\" when position > 0 then sell all stop close - 3 + bars_in_position * 2 because \"стоп\"\n",
      spec, timeline);
  const auto stops = ordersOf(out, "стоп");
  ASSERT_GE(stops.size(), 4u);
  EXPECT_EQ(stops[0]->spec.stopPrice, dec("97"));
  EXPECT_EQ(stops[0]->status, OrderStatus::Cancelled);
  EXPECT_EQ(stops[0]->reason, Reason::RuleReplaced);
  EXPECT_EQ(stops[1]->spec.stopPrice, dec("99"));
  EXPECT_EQ(stops[1]->reason, Reason::SessionEnd);  // дожила до конца сессии
  for (std::size_t i = 2; i < stops.size(); ++i) {
    EXPECT_EQ(stops[i]->status, OrderStatus::Rejected) << "заявка " << i;
    EXPECT_EQ(stops[i]->reason, Reason::InvalidParams) << "заявка " << i;
  }
  EXPECT_GE(out.rejections, 1);
}

TEST(Backtest, StopOnSameBarAsMarketExitDoesNotSellTwice) {
  // Рыночный выход и стоп поданы на одном баре и оба сработают на следующем: продажа одна, стоп закрывается
  // остатком с InsufficientPosition (причины PositionClosed у уже сработавшего на баре стопа нет).
  const auto [spec, timeline] = testing::syntheticTimeline({"100", "100", "100", "100"});
  const RunOutput out = run(
      "rule \"in\" when position == 0 then buy 1 lots because \"вход\"\n"
      "rule \"выход\" when bars_in_position == 0 then sell all because \"выход\"\n"
      "rule \"стоп\" when position > 0 then sell all stop 99.9 because \"стоп\"\n",
      spec, timeline);
  int sells = 0;
  for (const Fill& f : out.fills) sells += f.side == Side::Sell;
  EXPECT_EQ(sells, 1);
  const auto stops = ordersOf(out, "стоп");
  ASSERT_EQ(stops.size(), 1u);
  EXPECT_EQ(stops[0]->filledLots, 0);
  EXPECT_EQ(stops[0]->status, OrderStatus::Cancelled);
  EXPECT_EQ(stops[0]->reason, Reason::InsufficientPosition);
}

TEST(Backtest, BarsInPositionResetsAfterExitAndReentry) {
  // «Метка» ставится на баре, где позиция открыта (bars_in_position == 0): после выхода и повторного входа счёт
  // идёт заново, то есть метки появляются на каждом новом входе.
  const auto [spec, timeline] = testing::syntheticTimeline(std::vector<std::string>(12, "100"));
  const RunOutput out = run(
      "rule \"in\" when position == 0 then buy 1 lots because \"вход\"\n"
      "rule \"out\" when bars_in_position >= 2 then sell all because \"выход\"\n"
      "rule \"mark\" when bars_in_position == 0 then sell 1 lots limit 1000 because \"метка\"\n",
      spec, timeline);
  std::vector<std::int64_t> marks;
  for (const Order* o : ordersOf(out, "mark")) marks.push_back(o->placedStep);
  const std::vector<std::int64_t> expected{1, 5, 9};
  EXPECT_EQ(marks, expected);
}

TEST(RuntimeChecks, IntegrityViolationSendsFinalProgress) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto loaded = loadProgram(kSimpleRules);
  const detail::StepHook hook = [](const SessionEngine& engine) {
    if (engine.currentStep() == 3) throw IntegrityViolation("тест");
  };
  std::vector<std::pair<std::int64_t, std::int64_t>> calls;
  const RunOutput out = detail::runBacktestWithHook(
      spec, timeline, loaded.program, loaded.params, {}, dsl::Level::Ok,
      [&](std::int64_t done, std::int64_t total) { calls.emplace_back(done, total); }, hook);
  EXPECT_EQ(out.status, RunStatus::Invalid);
  const std::vector<std::pair<std::int64_t, std::int64_t>> expected{{4, 10}};
  EXPECT_EQ(calls, expected);
}

// Обработка, а не обнаружение: IntegrityViolation бросает тестовый StepHook (детекторы движка срабатывают только при
// ошибке в самом движке и недостижимы через публичный API). Проверяется, что прогон при таком исключении
// останавливается со статусом Invalid и находкой E-RT01.
TEST(RuntimeChecks, EngineIntegrityViolationInvalidatesRun) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto loaded = loadProgram(kSimpleRules);
  const detail::StepHook hook = [](const SessionEngine& engine) {
    if (engine.currentStep() == 3) throw IntegrityViolation("тестовый движок нарушил целостность");
  };
  const RunOutput out =
      detail::runBacktestWithHook(spec, timeline, loaded.program, loaded.params, {}, dsl::Level::Ok, nullptr, hook);

  EXPECT_EQ(out.status, RunStatus::Invalid);
  EXPECT_EQ(out.level, dsl::Level::Error);
  const Finding* f = finding(out, "E-RT01");
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->level, dsl::Level::Error);
  EXPECT_EQ(f->step, 3);
  EXPECT_EQ(out.lastStep, 3);
  EXPECT_EQ(out.equity.size(), 4u);  // прогон остановлен на шаге 3
  EXPECT_EQ(out.digest, digestOf(out));
}

}  // namespace
}  // namespace core
