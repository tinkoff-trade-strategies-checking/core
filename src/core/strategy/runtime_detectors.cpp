#include "core/strategy/runtime_detectors.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace core {
namespace {

struct GapGroup {
  std::int64_t NetVolume = 0;
  std::int64_t PlacedStep = -1;
};

} // namespace

CheckLevel combineCheckLevel(CheckLevel A, CheckLevel B) noexcept {
  if (A == CheckLevel::Error || B == CheckLevel::Error) {
    return CheckLevel::Error;
  }
  if (A == CheckLevel::Warning || B == CheckLevel::Warning) {
    return CheckLevel::Warning;
  }
  return CheckLevel::Ok;
}

CheckLevel
staticLookaheadLevel(const std::vector<dsl::Diagnostic> &Diagnostics) noexcept {
  CheckLevel Result = CheckLevel::Ok;
  for (const dsl::Diagnostic &Diag : Diagnostics) {
    if (Diag.Code.starts_with("E-LA")) {
      Result = combineCheckLevel(Result, CheckLevel::Error);
    } else if (Diag.Code.starts_with("W-LA")) {
      Result = combineCheckLevel(Result, CheckLevel::Warning);
    }
  }
  return Result;
}

std::optional<Finding> detectGuessedGaps(const Timeline &Tl,
                                         const std::vector<Order> &Orders,
                                         const std::vector<Fill> &Fills) {
  std::map<std::int64_t, const Order *> OrderById;
  for (const Order &Ord : Orders) {
    OrderById[Ord.Id] = &Ord;
  }

  std::map<std::pair<int, std::int64_t>, GapGroup> Groups;
  for (const Fill &FillItem : Fills) {
    const auto OrdIt = OrderById.find(FillItem.OrderId);
    if (OrdIt == OrderById.end()) {
      continue;
    }
    const Order *Ord = OrdIt->second;
    if (Ord->Spec.Type != OrderType::Market || Ord->RuleName.empty()) {
      continue;
    }

    GapGroup &Group = Groups[{FillItem.Instrument, FillItem.Step}];
    const std::int64_t SignedLots =
        (FillItem.OrderSide == Side::Buy) ? FillItem.Lots : -FillItem.Lots;
    Group.NetVolume += SignedLots;
    if (Group.PlacedStep < 0 || Ord->PlacedStep > Group.PlacedStep) {
      Group.PlacedStep = Ord->PlacedStep;
    }
  }

  const std::int64_t TotalSteps = Tl.totalSteps();
  std::int64_t TotalGaps = 0;
  std::int64_t GuessedGaps = 0;

  for (const auto &[Key, Group] : Groups) {
    if (Group.NetVolume == 0) {
      continue;
    }
    const int Instr = Key.first;
    const std::int64_t NextStep = Key.second;
    if (Group.PlacedStep < 0 || Group.PlacedStep >= TotalSteps ||
        NextStep < 0 || NextStep >= TotalSteps) {
      continue;
    }
    const StepBar *BarPlaced = Tl.barOf(Instr, Group.PlacedStep);
    const StepBar *BarNext = Tl.barOf(Instr, NextStep);
    if (BarPlaced == nullptr || BarNext == nullptr ||
        BarPlaced->Close.sign() <= 0) {
      continue;
    }

    const Decimal Gap = BarNext->Open - BarPlaced->Close;
    const Int128 AbsGapRaw = Gap.abs().raw();
    const Int128 CloseRaw = BarPlaced->Close.raw();
    const bool HasGap =
        AbsGapRaw >= CloseRaw || (AbsGapRaw * 100 >= CloseRaw * 2);
    if (!HasGap) {
      continue;
    }

    ++TotalGaps;
    if ((Group.NetVolume > 0 && Gap.sign() > 0) ||
        (Group.NetVolume < 0 && Gap.sign() < 0)) {
      ++GuessedGaps;
    }
  }

  if (GuessedGaps >= 5 && GuessedGaps * 5 >= TotalGaps * 4) {
    std::string Msg = "Подозрение на подглядывание в будущее: ";
    Msg += "угадано " + std::to_string(GuessedGaps);
    Msg += " из " + std::to_string(TotalGaps);
    Msg += " гэпов >= 2% рыночными заявками стратегии";
    return Finding{"W-RT01", CheckLevel::Warning, Msg, -1};
  }
  return std::nullopt;
}

std::optional<Finding>
detectTooSmoothResult(const std::vector<RoundTrip> &RoundTrips) {
  const std::size_t Total = RoundTrips.size();
  if (Total < 30) {
    return std::nullopt;
  }
  std::size_t Wins = 0;
  for (const RoundTrip &Trip : RoundTrips) {
    if (Trip.NetPnl.sign() > 0) {
      ++Wins;
    }
  }
  if (Wins * 10 > Total * 9) {
    const std::string Msg =
        "Слишком ровный результат: " + std::to_string(Wins) +
        " прибыльных сделок из " + std::to_string(Total) + " (> 90%)";
    return Finding{"W-RT02", CheckLevel::Warning, Msg, -1};
  }
  return std::nullopt;
}

void applyRuntimeChecks(Report &OutReport, CheckLevel StaticLevel,
                        const Timeline &Tl, const std::vector<Order> &Orders,
                        const std::vector<Fill> &Fills,
                        const std::vector<Finding> &ExtraFindings) {
  for (const Finding &Item : ExtraFindings) {
    OutReport.Findings.push_back(Item);
  }
  if (const auto GapsFinding = detectGuessedGaps(Tl, Orders, Fills)) {
    OutReport.Findings.push_back(*GapsFinding);
  }
  if (const auto SmoothFinding = detectTooSmoothResult(OutReport.RoundTrips)) {
    OutReport.Findings.push_back(*SmoothFinding);
  }

  CheckLevel Worst = combineCheckLevel(StaticLevel, OutReport.ReportLevel);
  for (const Finding &Item : OutReport.Findings) {
    Worst = combineCheckLevel(Worst, Item.Severity);
  }
  OutReport.ReportLevel = Worst;
}

} // namespace core
