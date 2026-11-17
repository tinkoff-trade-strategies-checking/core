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

RunOutput run(const std::string &Source, const SessionSpec &Spec,
              const std::shared_ptr<const Timeline> &Timeline,
              const dsl::ParamValues &Overrides = {},
              const Progress &Progress = nullptr) {
  const auto Loaded = loadProgram(Source);
  return runBacktest(Spec, Timeline, Loaded.Program, Loaded.Params, Overrides,
                     dsl::Level::Ok, Progress);
}

const Order *findOrder(const RunOutput &Out, std::int64_t Id) {
  for (const Order &O : Out.Orders) {
    if (O.Id == Id)
      return &O;
  }
  return nullptr;
}

std::vector<const Order *> ordersOf(const RunOutput &Out,
                                    const std::string &Rule) {
  std::vector<const Order *> Found;
  for (const Order &O : Out.Orders) {
    if (O.RuleName == Rule)
      Found.push_back(&O);
  }
  return Found;
}

constexpr const char *kSimpleRules =
    "rule \"b\" when close > close[1] and position == 0 then buy 10 lots "
    "because \"рост\"\n"
    "rule \"s\" when close < close[1] and position > 0 then sell all because "
    "\"падение\"\n";

TEST(Backtest, SimpleRulesOnFixture) {
  const auto [spec, timeline] = testing::fixtureA();
  const RunOutput Out = run(kSimpleRules, spec, timeline);

  EXPECT_EQ(Out.Status, RunStatus::Completed);
  EXPECT_EQ(Out.Severity, dsl::Level::Ok);
  EXPECT_EQ(Out.LastStep, 9);
  ASSERT_EQ(Out.Fills.size(), 3u);

  EXPECT_EQ(Out.Fills[0].Step, 2);
  EXPECT_EQ(Out.Fills[0].OrderSide, Side::Buy);
  EXPECT_EQ(Out.Fills[0].Lots, 10);
  EXPECT_EQ(Out.Fills[0].Price, dec("101.07")); // base 101, raw 101.0606

  EXPECT_EQ(Out.Fills[1].Step, 5);
  EXPECT_EQ(Out.Fills[1].OrderSide, Side::Sell);
  EXPECT_EQ(Out.Fills[1].Price, dec("103.93")); // raw 103.9376

  EXPECT_EQ(Out.Fills[2].Step, 8);
  EXPECT_EQ(Out.Fills[2].OrderSide, Side::Buy);
  EXPECT_EQ(Out.Fills[2].Price, dec("102.07")); // raw 102.0612

  const char *Because[] = {"рост", "падение", "рост"};
  for (std::size_t I = 0; I < 3; ++I) {
    const Order *Order = findOrder(Out, Out.Fills[I].OrderId);
    ASSERT_NE(Order, nullptr);
    EXPECT_EQ(Order->Because, Because[I]);
  }
  EXPECT_EQ(Out.Equity.size(), 10u);
  EXPECT_EQ(Out.PositionValuesByStep.size(), 10u);
  EXPECT_EQ(Out.FinalEquity, Out.Equity.back().Equity);
  EXPECT_EQ(Out.RoundTrips.size(), 1u);
  EXPECT_EQ(Out.Digest, digestOf(Out));
}

TEST(Backtest, SameEngineAsHuman) {
  // Стратегия с заменой и отменой заявок: журнал стратегии проигрывается
  // движком как журнал человека.
  const auto [spec, timeline] = testing::fixtureA();
  const RunOutput Out =
      run(std::string(kSimpleRules) +
              "rule \"lim\" when close < 103 then buy 1 lots limit close * 0.9 "
              "because \"лимит\"\n"
              "rule \"c\" when close >= 106 then cancel because \"отмена\"\n",
          spec, timeline);
  ASSERT_FALSE(Out.Journal.empty());

  const ReplayResult Replay =
      replaySession(spec, timeline, Out.Journal, Out.LastStep, 0);
  ASSERT_EQ(Replay.Engine.fills().size(), Out.Fills.size());
  for (std::size_t I = 0; I < Out.Fills.size(); ++I) {
    EXPECT_TRUE(testing::sameFill(Replay.Engine.fills()[I], Out.Fills[I]))
        << "сделка " << I;
  }
  ASSERT_EQ(Replay.Engine.equity().size(), Out.Equity.size());
  for (std::size_t I = 0; I < Out.Equity.size(); ++I) {
    EXPECT_EQ(Replay.Engine.equity()[I].Equity, Out.Equity[I].Equity)
        << "шаг " << I;
  }
  ASSERT_EQ(Replay.Engine.orders().size(), Out.Orders.size());
  for (std::size_t I = 0; I < Out.Orders.size(); ++I) {
    EXPECT_EQ(Replay.Engine.orders()[I].Status, Out.Orders[I].Status)
        << "заявка " << I;
    EXPECT_EQ(Replay.Engine.orders()[I].ReasonCode, Out.Orders[I].ReasonCode)
        << "заявка " << I;
    EXPECT_EQ(Replay.Engine.orders()[I].ClosedStep, Out.Orders[I].ClosedStep)
        << "заявка " << I;
  }
}

TEST(Backtest, StopCancelledWhenFlat) {
  const auto [spec, timeline] = testing::fixtureA();
  const RunOutput Out =
      run("rule \"вход\" when close > close[1] and position == 0 then buy 10 "
          "lots because \"рост\"\n"
          "rule \"стоп\" when position > 0 then sell all stop entry_price * "
          "0.5 because \"защитный стоп\"\n"
          "rule \"перекупленность\" when position > 0 and close > 105 then "
          "sell all because \"перекупленность\"\n",
          spec, timeline);

  // Выход по перекупленности: решение на шаге 3 (close 106), продажа на шаге 4;
  // стоп снят с PositionClosed.
  const auto Stops = ordersOf(Out, "стоп");
  ASSERT_GE(Stops.size(), 1u);
  EXPECT_EQ(Stops[0]->Status, OrderStatus::Cancelled);
  EXPECT_EQ(Stops[0]->ReasonCode, Reason::PositionClosed);
  EXPECT_EQ(Stops[0]->ClosedStep, 4);
  ASSERT_GE(Out.Fills.size(), 2u);
  EXPECT_EQ(Out.Fills[1].OrderSide, Side::Sell);
  EXPECT_EQ(Out.Fills[1].Step, 4);
  // В журнале это запись CancelCmd с причиной PositionClosed.
  bool Journaled = false;
  for (const JournalEntry &E : Out.Journal) {
    if (const auto *Cancel = std::get_if<CancelCmd>(&E.Command)) {
      Journaled |= Cancel->OrderId == Stops[0]->Id &&
                   Cancel->ReasonCode == Reason::PositionClosed && E.Step == 4;
    }
  }
  EXPECT_TRUE(Journaled);
}

TEST(Backtest, OneActiveOrderPerRule) {
  const auto [spec, timeline] = testing::fixtureA();
  // «moving»: цена лимита меняется каждый бар - прежняя заявка снимается.
  // «fixed»: заявка не меняется - не дублируется.
  const RunOutput Out = run("rule \"moving\" when true then buy 1 lots limit "
                            "close * 0.5 because \"подвижная\"\n"
                            "rule \"fixed\" when true then buy 1 lots limit 50 "
                            "because \"постоянная\"\n",
                            spec, timeline);

  const auto FixedOrders = ordersOf(Out, "fixed");
  ASSERT_EQ(FixedOrders.size(), 1u);
  EXPECT_EQ(FixedOrders[0]->PlacedStep, 0);

  const auto Moving = ordersOf(Out, "moving");
  ASSERT_EQ(Moving.size(),
            9u); // шаги 0…8; на последнем шаге 9 сессия уже закончена
  for (std::size_t I = 0; I < Moving.size(); ++I) {
    EXPECT_EQ(Moving[I]->Status, OrderStatus::Cancelled);
    EXPECT_EQ(Moving[I]->ReasonCode,
              I + 1 < Moving.size() ? Reason::RuleReplaced : Reason::SessionEnd)
        << "заявка " << I;
  }
  // На любом шаге у правила не больше одной активной заявки.
  for (std::int64_t Step = 0; Step <= Out.LastStep; ++Step) {
    int Active = 0;
    for (const Order *O : Moving)
      Active +=
          O->PlacedStep <= Step && (O->ClosedStep < 0 || O->ClosedStep > Step);
    EXPECT_LE(Active, 1) << "шаг " << Step;
  }
}

TEST(Backtest, CancelActionCancelsRestingOrders) {
  const auto [spec, timeline] = testing::fixtureA();
  const RunOutput Out =
      run("rule \"lim\" when close < 103 then buy 1 lots limit 50 because "
          "\"лимит\"\n"
          "rule \"c\" when close >= 103 then cancel because \"отмена\"\n",
          spec, timeline);
  const auto Lim = ordersOf(Out, "lim");
  ASSERT_GE(Lim.size(), 1u);
  EXPECT_EQ(Lim[0]->Status, OrderStatus::Cancelled);
  EXPECT_EQ(Lim[0]->ReasonCode, Reason::CancelledByUser);
  EXPECT_EQ(Lim[0]->ClosedStep, 2);
}

TEST(Backtest, EachInstrumentGetsOwnOrder) {
  auto F = testing::fixture1();
  const auto Line = std::make_shared<const Timeline>(
      Timeline::load(F.Bars, F.Calendar, F.SnapshotId, F.Instruments,
                     Timeframe::D1, 20240109, 20240122));
  const RunOutput Out =
      run("rule \"b\" when position == 0 then buy 1 lots because \"вход\"\n",
          testing::specFor(F.Instruments), Line);
  bool A = false, B = false;
  for (const Fill &Fill : Out.Fills) {
    A |= Fill.Instrument == 0;
    B |= Fill.Instrument == 1;
  }
  EXPECT_TRUE(A);
  EXPECT_TRUE(B);
  EXPECT_EQ(Out.Fills.size(), 2u);
}

TEST(Backtest, ProgressEveryThousandStepsAndAtEnd) {
  const auto [spec, timeline] =
      testing::syntheticTimeline(testing::randomWalk(2500));
  std::vector<std::pair<std::int64_t, std::int64_t>> Calls;
  run(kSimpleRules, spec, timeline, {},
      [&](std::int64_t Done, std::int64_t Total) {
        Calls.emplace_back(Done, Total);
      });
  const std::vector<std::pair<std::int64_t, std::int64_t>> Expected{
      {1000, 2500}, {2000, 2500}, {2500, 2500}};
  EXPECT_EQ(Calls, Expected);
}

TEST(Backtest, RefusesBadOverride) {
  // INV-1: экземпляр стратегии заново проверяет программу; значение вне
  // диапазона - отказ, не прогон.
  const auto [spec, timeline] = testing::fixtureA();
  const std::string Source = "param n = 2 range 1..5 step 1\n"
                             "rule \"b\" when close > sma(close, n) then buy 1 "
                             "lots because \"выше средней\"\n";
  EXPECT_THROW(run(Source, spec, timeline, {{"n", 0}}), std::invalid_argument);
  EXPECT_THROW(run(Source, spec, timeline, {{"n", 100000}}),
               std::invalid_argument);
}

TEST(Backtest, StaticLevelCarriesIntoRunLevel) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto Loaded = loadProgram(kSimpleRules);
  const RunOutput Out =
      runBacktest(spec, timeline, Loaded.Program, Loaded.Params, {},
                  dsl::Level::Warning, nullptr);
  EXPECT_EQ(Out.Severity, dsl::Level::Warning);
  EXPECT_TRUE(Out.Findings.empty());
}

TEST(Backtest, CountsRejections) {
  // Покупка на сумму больше капитала не проходит проверку денег при подаче:
  // отказ попадает в счётчик.
  const auto [spec, timeline] = testing::fixtureA();
  const RunOutput Out =
      run("rule \"b\" when true then buy 99999999 lots because \"много\"\n",
          spec, timeline);
  EXPECT_GT(Out.Rejections, 0);
  EXPECT_EQ(Out.Fills.size(), 0u);
  EXPECT_EQ(Out.Orders.front().Status, OrderStatus::Rejected);
}

// Правила «угадывателя»: купить на низком close (< 100), продать на высоком (>
// 100).
constexpr const char *kFlipRules = "rule \"b\" when close < 100 and position "
                                   "== 0 then buy 1 lots because \"низко\"\n"
                                   "rule \"s\" when close > 100 and position > "
                                   "0 then sell all because \"высоко\"\n";

const Finding *finding(const RunOutput &Out, const std::string &Code) {
  for (const Finding &F : Out.Findings) {
    if (F.Code == Code)
      return &F;
  }
  return nullptr;
}

// Бары с гэпами: нечётные бары открываются с гэпом вверх (+4 % к close
// предыдущего), чётные - с гэпом вниз.
std::pair<SessionSpec, std::shared_ptr<const Timeline>>
gappyTimeline(std::size_t Bars, bool UpThenDown) {
  std::vector<std::string> Closes, Opens;
  for (std::size_t I = 0; I < Bars; ++I) {
    const bool Low = (I % 2 == 0) ==
                     UpThenDown; // «низкий» бар: close 99, «высокий»: close 104
    Closes.push_back(Low ? "99" : "104");
    Opens.push_back(Low ? "99" : "103");
  }
  return testing::syntheticTimeline(Closes, Opens);
}

TEST(RuntimeChecks, GapLuckWarning) {
  // Шесть гэпов вверх: покупка за бар до каждого (и продажи перед гэпами вниз
  // между ними).
  const auto [spec, timeline] = gappyTimeline(14, true);
  const RunOutput Out = run(kFlipRules, spec, timeline);
  const Finding *F = finding(Out, "W-RT01");
  ASSERT_NE(F, nullptr);
  EXPECT_EQ(F->Severity, dsl::Level::Warning);
  EXPECT_EQ(Out.Severity, dsl::Level::Warning);
  EXPECT_EQ(Out.Status,
            RunStatus::Completed); // предупреждение прогон не останавливает
}

TEST(RuntimeChecks, GapsNotGuessedGiveNoWarning) {
  // Тот же рисунок цен, но стратегия торгует против гэпов: покупка перед гэпом
  // вниз, продажа перед гэпом вверх.
  const auto [spec, timeline] = gappyTimeline(14, false);
  const RunOutput Out = run("rule \"b\" when close > 100 and position == 0 "
                            "then buy 1 lots because \"высоко\"\n"
                            "rule \"s\" when close < 100 and position > 0 then "
                            "sell all because \"низко\"\n",
                            spec, timeline);
  EXPECT_GE(Out.Fills.size(), 5u);
  EXPECT_EQ(finding(Out, "W-RT01"), nullptr);
  EXPECT_EQ(Out.Severity, dsl::Level::Ok);
}

TEST(RuntimeChecks, TooEvenResultWarning) {
  // Растущий рынок (+2 % за бар), сделка на каждом баре: все закрытые пары
  // прибыльные.
  std::vector<std::string> Closes;
  double Price = 100;
  for (int I = 0; I < 90; ++I) {
    char Text[32];
    std::snprintf(Text, sizeof Text, "%.2f", Price);
    Closes.emplace_back(Text);
    Price *= 1.02;
  }
  const auto [spec, timeline] = testing::syntheticTimeline(Closes);
  const std::string Rules =
      "rule \"b\" when position == 0 then buy 1 lots because \"вход\"\n"
      "rule \"s\" when position > 0 then sell all because \"выход\"\n";
  const RunOutput Out = run(Rules, spec, timeline);
  ASSERT_GE(Out.RoundTrips.size(), 30u);
  const Finding *F = finding(Out, "W-RT02");
  ASSERT_NE(F, nullptr);
  EXPECT_EQ(F->Severity, dsl::Level::Warning);

  // Меньше 30 закрытых пар - предупреждения нет.
  const auto [shortSpec, shortTimeline] = testing::syntheticTimeline(
      std::vector<std::string>(Closes.begin(), Closes.begin() + 30));
  const RunOutput ShortRun = run(Rules, shortSpec, shortTimeline);
  EXPECT_LT(ShortRun.RoundTrips.size(), 30u);
  EXPECT_EQ(finding(ShortRun, "W-RT02"), nullptr);
}

// Гэпы на границах порогов: total гэпов вверх (close 100 -> open 103), перед
// guessed из них - покупка, перед остальными - продажа. Фактически заявки и
// сделки собраны вручную: проверяется сама проверка, не стратегия.
std::vector<Finding> gapFindings(int Guessed, int Total) {
  std::vector<std::string> Closes, Opens;
  for (int J = 0; J < Total; ++J) {
    Closes.insert(Closes.end(), {"100", "103"});
    Opens.insert(Opens.end(), {J == 0 ? "100" : "103", "103"});
  }
  const auto [spec, timeline] = testing::syntheticTimeline(Closes, Opens);
  std::vector<Order> Orders;
  std::vector<Fill> Fills;
  for (int J = 0; J < Total; ++J) {
    const Side Side = J < Guessed ? Side::Buy : Side::Sell;
    Order Order;
    Order.Id = J * 2;
    Order.Spec.OrderSide = Side;
    Order.Spec.Lots = 1;
    Order.PlacedStep = 2 * J;
    Orders.push_back(Order);
    Fill Fill;
    Fill.OrderId = Order.Id;
    Fill.Step = 2 * J + 1;
    Fill.OrderSide = Side;
    Fill.Lots = 1;
    Fill.BasePrice = Fill.Price = dec("103");
    Fills.push_back(Fill);
  }
  return detail::runtimeFindings(*timeline, Orders, Fills, {});
}

TEST(RuntimeChecks, GapThresholdsAreInclusiveOnShareAndCount) {
  EXPECT_TRUE(gapFindings(4, 4).empty()); // 100 %, но угаданных меньше 5
  EXPECT_EQ(gapFindings(5, 5).size(), 1u);   // ровно 5
  EXPECT_TRUE(gapFindings(15, 19).empty());  // 78,9 % < 80 %
  EXPECT_EQ(gapFindings(16, 20).size(), 1u); // ровно 80 %
}

std::vector<Finding> evenFindings(int Trips, int Wins) {
  const auto [spec, timeline] = testing::fixtureA();
  std::vector<RoundTrip> RoundTrips;
  for (int I = 0; I < Trips; ++I) {
    RoundTrip Trip{};
    Trip.ExitStep = I;
    Trip.NetPnl = I < Wins ? dec("10") : dec("-10");
    RoundTrips.push_back(Trip);
  }
  return detail::runtimeFindings(*timeline, {}, {}, RoundTrips);
}

TEST(RuntimeChecks, TooEvenThresholds) {
  EXPECT_TRUE(evenFindings(29, 29).empty()); // меньше 30 пар
  EXPECT_TRUE(evenFindings(30, 27).empty()); // ровно 90 % - строго больше нужно
  EXPECT_EQ(evenFindings(30, 28).size(), 1u); // 93 %
  EXPECT_TRUE(evenFindings(30, 29).size() == 1u);
}

TEST(Backtest, RejectedReplacementKeepsOldOrder) {
  // Стоп правила «стоп» с каждым баром отодвигается вверх и на третьем баре
  // позиции оказывается выше close - новая заявка отклонена, а прежняя
  // остаётся: у правила не пропадает защита.
  const auto [spec, timeline] =
      testing::syntheticTimeline(std::vector<std::string>(9, "100"));
  const RunOutput Out =
      run("rule \"in\" when position == 0 then buy 1 lots because \"вход\"\n"
          "rule \"стоп\" when position > 0 then sell all stop close - 3 + "
          "bars_in_position * 2 because \"стоп\"\n",
          spec, timeline);
  const auto Stops = ordersOf(Out, "стоп");
  ASSERT_GE(Stops.size(), 4u);
  EXPECT_EQ(Stops[0]->Spec.StopPrice, dec("97"));
  EXPECT_EQ(Stops[0]->Status, OrderStatus::Cancelled);
  EXPECT_EQ(Stops[0]->ReasonCode, Reason::RuleReplaced);
  EXPECT_EQ(Stops[1]->Spec.StopPrice, dec("99"));
  EXPECT_EQ(Stops[1]->ReasonCode, Reason::SessionEnd); // дожила до конца сессии
  for (std::size_t I = 2; I < Stops.size(); ++I) {
    EXPECT_EQ(Stops[I]->Status, OrderStatus::Rejected) << "заявка " << I;
    EXPECT_EQ(Stops[I]->ReasonCode, Reason::InvalidParams) << "заявка " << I;
  }
  EXPECT_GE(Out.Rejections, 1);
}

TEST(Backtest, StopOnSameBarAsMarketExitDoesNotSellTwice) {
  // Рыночный выход и стоп поданы на одном баре и оба сработают на следующем:
  // продажа одна, стоп закрывается остатком с InsufficientPosition (причины
  // PositionClosed у уже сработавшего на баре стопа нет).
  const auto [spec, timeline] =
      testing::syntheticTimeline({"100", "100", "100", "100"});
  const RunOutput Out =
      run("rule \"in\" when position == 0 then buy 1 lots because \"вход\"\n"
          "rule \"выход\" when bars_in_position == 0 then sell all because "
          "\"выход\"\n"
          "rule \"стоп\" when position > 0 then sell all stop 99.9 because "
          "\"стоп\"\n",
          spec, timeline);
  int Sells = 0;
  for (const Fill &F : Out.Fills)
    Sells += F.OrderSide == Side::Sell;
  EXPECT_EQ(Sells, 1);
  const auto Stops = ordersOf(Out, "стоп");
  ASSERT_EQ(Stops.size(), 1u);
  EXPECT_EQ(Stops[0]->FilledLots, 0);
  EXPECT_EQ(Stops[0]->Status, OrderStatus::Cancelled);
  EXPECT_EQ(Stops[0]->ReasonCode, Reason::InsufficientPosition);
}

TEST(Backtest, BarsInPositionResetsAfterExitAndReentry) {
  // «Метка» ставится на баре, где позиция открыта (bars_in_position == 0):
  // после выхода и повторного входа счёт идёт заново, то есть метки появляются
  // на каждом новом входе.
  const auto [spec, timeline] =
      testing::syntheticTimeline(std::vector<std::string>(12, "100"));
  const RunOutput Out =
      run("rule \"in\" when position == 0 then buy 1 lots because \"вход\"\n"
          "rule \"out\" when bars_in_position >= 2 then sell all because "
          "\"выход\"\n"
          "rule \"mark\" when bars_in_position == 0 then sell 1 lots limit "
          "1000 because \"метка\"\n",
          spec, timeline);
  std::vector<std::int64_t> Marks;
  for (const Order *O : ordersOf(Out, "mark"))
    Marks.push_back(O->PlacedStep);
  const std::vector<std::int64_t> Expected{1, 5, 9};
  EXPECT_EQ(Marks, Expected);
}

TEST(RuntimeChecks, IntegrityViolationSendsFinalProgress) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto Loaded = loadProgram(kSimpleRules);
  const detail::StepHook Hook = [](const SessionEngine &Engine) {
    if (Engine.currentStep() == 3)
      throw IntegrityViolation("тест");
  };
  std::vector<std::pair<std::int64_t, std::int64_t>> Calls;
  const RunOutput Out = detail::runBacktestWithHook(
      spec, timeline, Loaded.Program, Loaded.Params, {}, dsl::Level::Ok,
      [&](std::int64_t Done, std::int64_t Total) {
        Calls.emplace_back(Done, Total);
      },
      Hook);
  EXPECT_EQ(Out.Status, RunStatus::Invalid);
  const std::vector<std::pair<std::int64_t, std::int64_t>> Expected{{4, 10}};
  EXPECT_EQ(Calls, Expected);
}

// Обработка, а не обнаружение: IntegrityViolation бросает тестовый StepHook
// (детекторы движка срабатывают только при ошибке в самом движке и недостижимы
// через публичный API). Проверяется, что прогон при таком исключении
// останавливается со статусом Invalid и находкой E-RT01.
TEST(RuntimeChecks, EngineIntegrityViolationInvalidatesRun) {
  const auto [spec, timeline] = testing::fixtureA();
  const auto Loaded = loadProgram(kSimpleRules);
  const detail::StepHook Hook = [](const SessionEngine &Engine) {
    if (Engine.currentStep() == 3)
      throw IntegrityViolation("тестовый движок нарушил целостность");
  };
  const RunOutput Out =
      detail::runBacktestWithHook(spec, timeline, Loaded.Program, Loaded.Params,
                                  {}, dsl::Level::Ok, nullptr, Hook);

  EXPECT_EQ(Out.Status, RunStatus::Invalid);
  EXPECT_EQ(Out.Severity, dsl::Level::Error);
  const Finding *F = finding(Out, "E-RT01");
  ASSERT_NE(F, nullptr);
  EXPECT_EQ(F->Severity, dsl::Level::Error);
  EXPECT_EQ(F->Step, 3);
  EXPECT_EQ(Out.LastStep, 3);
  EXPECT_EQ(Out.Equity.size(), 4u); // прогон остановлен на шаге 3
  EXPECT_EQ(Out.Digest, digestOf(Out));
}

} // namespace
} // namespace core
