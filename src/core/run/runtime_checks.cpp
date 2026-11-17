#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <utility>

#include "core/run/backtest.hpp"

// Проверки во время исполнения (dsl.md, «Чтение будущего», таблица «Во время
// исполнения»).

namespace core::detail {

namespace {

// Пороги из dsl.md.
constexpr std::int64_t kGapPercent = 2;    // |open(t+1) / close(t) - 1| >= 2 %
constexpr std::int64_t kGapMinGuessed = 5; // угаданных гэпов >= 5
constexpr std::int64_t kGapMinSharePercent =
    80; // и >= 80 % гэпов с исполненной заявкой перед ними
constexpr std::int64_t kEvenMinTrips = 30; // закрытых сделок >= 30
constexpr std::int64_t kEvenMinWinPercent = 90; // доля прибыльных > 90 %

const Order *findOrder(const std::vector<Order> &Orders, std::int64_t Id) {
  const auto It = std::lower_bound(
      Orders.begin(), Orders.end(), Id,
      [](const Order &O, std::int64_t Value) { return O.Id < Value; });
  return It != Orders.end() && It->Id == Id ? &*It : nullptr;
}

// |open - close| / close >= 2 % без double: |open - close| * 100 >= 2 * close.
bool isGap(const Decimal &Open, const Decimal &PrevClose) {
  if (PrevClose.sign() <= 0)
    return false;
  return (Open - PrevClose).abs().times(100) >= PrevClose.times(kGapPercent);
}

// Находка W-RT01: гэпы, перед которыми стратегия создала исполненную заявку, и
// из них угаданные. Знаменатель - гэпы, перед которыми заявка подана на баре t
// и исполнена на баре t + 1 (заявки, поданные раньше и исполненные на t + 1,
// например лимитные, в него не входят): так задумано, `dsl.md` уточняет
// формулировку (задача 54).
void checkGuessedGaps(const Timeline &Timeline,
                      const std::vector<Order> &Orders,
                      const std::vector<Fill> &Fills,
                      std::vector<Finding> &Out) {
  struct Gap {
    bool Guessed = false;
    std::int64_t Step = 0; // шаг бара t + 1
  };
  std::map<std::pair<int, std::int64_t>, Gap> Gaps; // (бумага, шаг бара t)
  for (const Fill &Fill : Fills) {
    const Order *Order = findOrder(Orders, Fill.OrderId);
    const StepBar *Bar = Timeline.barOf(Fill.Instrument, Fill.Step);
    if (Order == nullptr || Bar == nullptr || Fill.Step < 1)
      continue;
    // Бар t - предыдущий бар бумаги; заявка должна быть подана на нём.
    const StepBar *Prev = Timeline.lastBarOf(Fill.Instrument, Fill.Step - 1);
    if (Prev == nullptr || Order->PlacedStep != Prev->Step)
      continue;
    if (!isGap(Bar->Open, Prev->Close))
      continue;
    Gap &Gap = Gaps[{Fill.Instrument, Prev->Step}];
    Gap.Step = Fill.Step;
    const bool Up = Bar->Open > Prev->Close;
    const bool AtOpen = Fill.BasePrice == Bar->Open;
    if (AtOpen && ((Fill.OrderSide == Side::Buy && Up) ||
                   (Fill.OrderSide == Side::Sell && !Up)))
      Gap.Guessed = true;
  }
  std::int64_t Guessed = 0, LastStep = 0;
  for (const auto &[key, gap] : Gaps) {
    if (gap.Guessed)
      ++Guessed;
    LastStep = std::max(LastStep, gap.Step);
  }
  const auto Total = static_cast<std::int64_t>(Gaps.size());
  if (Guessed >= kGapMinGuessed &&
      Guessed * 100 >= Total * kGapMinSharePercent) {
    Out.push_back(
        Finding{"W-RT01", dsl::Level::Warning,
                "Стратегия угадала гэпы: " + std::to_string(Guessed) + " из " +
                    std::to_string(Total) +
                    " (покупка перед гэпом вверх, продажа перед гэпом вниз)",
                LastStep});
  }
}

// Находка W-RT02: слишком ровный результат.
void checkTooEven(const std::vector<RoundTrip> &RoundTrips,
                  std::vector<Finding> &Out) {
  const auto Total = static_cast<std::int64_t>(RoundTrips.size());
  if (Total < kEvenMinTrips)
    return;
  std::int64_t Wins = 0, LastStep = 0;
  for (const RoundTrip &Trip : RoundTrips) {
    if (Trip.NetPnl.sign() > 0)
      ++Wins;
    LastStep = std::max(LastStep, Trip.ExitStep);
  }
  if (Wins * 100 > Total * kEvenMinWinPercent) {
    Out.push_back(
        Finding{"W-RT02", dsl::Level::Warning,
                "Слишком ровный результат: прибыльных закрытых сделок " +
                    std::to_string(Wins) + " из " + std::to_string(Total),
                LastStep});
  }
}

} // namespace

std::vector<Finding> runtimeFindings(const Timeline &Timeline,
                                     const std::vector<Order> &Orders,
                                     const std::vector<Fill> &Fills,
                                     const std::vector<RoundTrip> &RoundTrips) {
  std::vector<Finding> Out;
  checkGuessedGaps(Timeline, Orders, Fills, Out);
  checkTooEven(RoundTrips, Out);
  return Out;
}

Finding integrityFinding(const IntegrityViolation &Violation,
                         std::int64_t Step) {
  return Finding{"E-RT01", dsl::Level::Error,
                 std::string("Нарушена целостность времени в движке: ") +
                     Violation.what() + ". Прогон остановлен",
                 Step};
}

dsl::Level worstLevel(dsl::Level StaticLevel,
                      const std::vector<Finding> &Findings) {
  dsl::Level Severity = StaticLevel;
  for (const Finding &F : Findings)
    Severity = std::max(Severity, F.Severity);
  return Severity;
}

} // namespace core::detail
