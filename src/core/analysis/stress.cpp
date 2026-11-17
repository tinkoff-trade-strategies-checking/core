#include "core/analysis/stress.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace core {
namespace {

constexpr RoundingMode kHalfUp = RoundingMode::HalfUp;

void validate(const StressScenario &S) {
  if (S.StartStep < 0)
    throw std::invalid_argument("стресс: startStep должен быть >= 0");
  if (S.GapPercent < Decimal::zero() ||
      S.GapPercent >= Decimal::fromUnits(100)) {
    throw std::invalid_argument("стресс: гэп X должен быть в [0; 100)");
  }
  if (S.VolMultiplier < Decimal::zero())
    throw std::invalid_argument("стресс: множитель k должен быть >= 0");
  if (S.VolBars < 0)
    throw std::invalid_argument("стресс: число баров N должно быть >= 0");
  if (S.VolumeShare < Decimal::zero() || S.VolumeShare > Decimal::one()) {
    throw std::invalid_argument("стресс: доля объёма q должна быть в [0; 1]");
  }
}

// Цена до шага цены HALF_UP; <= 0 - шаг цены.
Decimal snapPrice(const Decimal &Price, const Decimal &Step) {
  const Decimal Rounded = Price.roundToStep(Step, kHalfUp);
  return Rounded > Decimal::zero() ? Rounded : Step;
}

// floor(volume * share) лотов, точно, без Decimal.
std::int64_t scaledVolume(std::int64_t Volume, const Decimal &Share) {
  const Int128 Product = static_cast<Int128>(Volume) * Share.raw();
  return static_cast<std::int64_t>(Product / Decimal::kUnit);
}

struct Ohlc {
  Decimal Open, High, Low, Close;
};

// Гэп: цены умножаются на (1 - X/100).
Ohlc gapped(const StepBar &Bar, const Decimal &GapFactor) {
  return {Bar.Open.mul(GapFactor, kHalfUp), Bar.High.mul(GapFactor, kHalfUp),
          Bar.Low.mul(GapFactor, kHalfUp), Bar.Close.mul(GapFactor, kHalfUp)};
}

// y' = c'_prev * (1 + k * (y / c_prev - 1)), каждая операция - 9 знаков
// HALF_UP.
Decimal volatilePrice(const Decimal &Y, const Decimal &PrevClose,
                      const Decimal &PrevStressedClose, const Decimal &K,
                      const Decimal &Step) {
  const Decimal Ratio = Y.div(PrevClose, kHalfUp);
  const Decimal Factor =
      Decimal::one() + (Ratio - Decimal::one()).mul(K, kHalfUp);
  return snapPrice(PrevStressedClose.mul(Factor, kHalfUp), Step);
}

void stressInstrument(const Timeline &Source, Timeline &Result, int Instrument,
                      const StressScenario &S, const Decimal &GapFactor) {
  const Decimal Step =
      Source.instruments()[static_cast<std::size_t>(Instrument)].PriceStep;
  const std::int64_t Total = Source.totalSteps();

  // Бары бумаги на шагах >= startStep.
  std::vector<const StepBar *> Bars;
  for (std::int64_t St = S.StartStep; St < Total; ++St) {
    if (const StepBar *B = Source.barOf(Instrument, St))
      Bars.push_back(B);
  }
  if (Bars.empty())
    return;

  const std::size_t Window =
      std::min<std::size_t>(static_cast<std::size_t>(S.VolBars), Bars.size());

  // c_{t0-1} (до события данные не меняются); нет бара - open первого бара t0.
  const StepBar *Prev =
      S.StartStep > 0 ? Source.lastBarOf(Instrument, S.StartStep - 1) : nullptr;
  Decimal PrevClose = Prev != nullptr ? Prev->Close : Bars.front()->Open;
  Decimal PrevStressedClose = PrevClose;

  Decimal FactorAfter = Decimal::one(); // f
  for (std::size_t I = 0; I < Bars.size(); ++I) {
    const StepBar &Orig = *Bars[I];
    StepBar &Out = Result.mutableBar(Orig.Step, Instrument);
    const Ohlc G = gapped(Orig, GapFactor);

    if (I < Window) {
      const Decimal O = volatilePrice(G.Open, PrevClose, PrevStressedClose,
                                      S.VolMultiplier, Step);
      const Decimal H = volatilePrice(G.High, PrevClose, PrevStressedClose,
                                      S.VolMultiplier, Step);
      const Decimal L = volatilePrice(G.Low, PrevClose, PrevStressedClose,
                                      S.VolMultiplier, Step);
      const Decimal C = volatilePrice(G.Close, PrevClose, PrevStressedClose,
                                      S.VolMultiplier, Step);
      Out.Open = O;
      Out.Close = C;
      Out.High = std::max({O, H, L, C});
      Out.Low = std::min({O, H, L, C});
      Out.VolumeLots = scaledVolume(Orig.VolumeLots, S.VolumeShare);
      PrevClose = G.Close;
      PrevStressedClose = C;
      if (I + 1 == Window && !G.Close.isZero())
        FactorAfter = C.div(G.Close, kHalfUp);
    } else {
      Out.Open = snapPrice(G.Open.mul(FactorAfter, kHalfUp), Step);
      Out.High = snapPrice(G.High.mul(FactorAfter, kHalfUp), Step);
      Out.Low = snapPrice(G.Low.mul(FactorAfter, kHalfUp), Step);
      Out.Close = snapPrice(G.Close.mul(FactorAfter, kHalfUp), Step);
    }
  }
}

} // namespace

Timeline applyStress(const Timeline &Input, const StressScenario &Scenario) {
  validate(Scenario);
  Timeline Result = Input;
  const Decimal GapFactor =
      Decimal::one() -
      Scenario.GapPercent.div(Decimal::fromUnits(100), kHalfUp);
  const int Count = static_cast<int>(Input.instruments().size());
  for (int I = 0; I < Count; ++I)
    stressInstrument(Input, Result, I, Scenario, GapFactor);
  return Result;
}

} // namespace core
