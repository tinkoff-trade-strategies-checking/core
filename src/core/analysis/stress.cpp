#include "core/analysis/stress.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace core {
namespace {

constexpr RoundingMode kHalfUp = RoundingMode::HALF_UP;

void validate(const StressScenario& s) {
  if (s.startStep < 0) throw std::invalid_argument("стресс: startStep должен быть >= 0");
  if (s.gapPercent < Decimal::zero() || s.gapPercent >= Decimal::fromUnits(100)) {
    throw std::invalid_argument("стресс: гэп X должен быть в [0; 100)");
  }
  if (s.volMultiplier < Decimal::zero()) throw std::invalid_argument("стресс: множитель k должен быть >= 0");
  if (s.volBars < 0) throw std::invalid_argument("стресс: число баров N должно быть >= 0");
  if (s.volumeShare < Decimal::zero() || s.volumeShare > Decimal::one()) {
    throw std::invalid_argument("стресс: доля объёма q должна быть в [0; 1]");
  }
}

// Цена до шага цены HALF_UP; <= 0 - шаг цены.
Decimal snapPrice(const Decimal& price, const Decimal& step) {
  const Decimal rounded = price.roundToStep(step, kHalfUp);
  return rounded > Decimal::zero() ? rounded : step;
}

// floor(volume * share) лотов, точно, без Decimal.
std::int64_t scaledVolume(std::int64_t volume, const Decimal& share) {
  const Int128 product = static_cast<Int128>(volume) * share.raw();
  return static_cast<std::int64_t>(product / Decimal::kUnit);
}

struct Ohlc {
  Decimal open, high, low, close;
};

// Гэп: цены умножаются на (1 - X/100).
Ohlc gapped(const StepBar& bar, const Decimal& gapFactor) {
  return {bar.open.mul(gapFactor, kHalfUp), bar.high.mul(gapFactor, kHalfUp), bar.low.mul(gapFactor, kHalfUp),
          bar.close.mul(gapFactor, kHalfUp)};
}

// y' = c'_prev * (1 + k * (y / c_prev - 1)), каждая операция - 9 знаков HALF_UP.
Decimal volatilePrice(const Decimal& y, const Decimal& prevClose, const Decimal& prevStressedClose,
                      const Decimal& k, const Decimal& step) {
  const Decimal ratio = y.div(prevClose, kHalfUp);
  const Decimal factor = Decimal::one() + (ratio - Decimal::one()).mul(k, kHalfUp);
  return snapPrice(prevStressedClose.mul(factor, kHalfUp), step);
}

void stressInstrument(const Timeline& source, Timeline& result, int instrument, const StressScenario& s,
                      const Decimal& gapFactor) {
  const Decimal step = source.instruments()[static_cast<std::size_t>(instrument)].priceStep;
  const std::int64_t total = source.totalSteps();

  // Бары бумаги на шагах >= startStep.
  std::vector<const StepBar*> bars;
  for (std::int64_t st = s.startStep; st < total; ++st) {
    if (const StepBar* b = source.barOf(instrument, st)) bars.push_back(b);
  }
  if (bars.empty()) return;

  const std::size_t window = std::min<std::size_t>(static_cast<std::size_t>(s.volBars), bars.size());

  // c_{t0-1} (до события данные не меняются); нет бара - open первого бара t0.
  const StepBar* prev = s.startStep > 0 ? source.lastBarOf(instrument, s.startStep - 1) : nullptr;
  Decimal prevClose = prev != nullptr ? prev->close : bars.front()->open;
  Decimal prevStressedClose = prevClose;

  Decimal factorAfter = Decimal::one();  // f
  for (std::size_t i = 0; i < bars.size(); ++i) {
    const StepBar& orig = *bars[i];
    StepBar& out = result.mutableBar(orig.step, instrument);
    const Ohlc g = gapped(orig, gapFactor);

    if (i < window) {
      const Decimal o = volatilePrice(g.open, prevClose, prevStressedClose, s.volMultiplier, step);
      const Decimal h = volatilePrice(g.high, prevClose, prevStressedClose, s.volMultiplier, step);
      const Decimal l = volatilePrice(g.low, prevClose, prevStressedClose, s.volMultiplier, step);
      const Decimal c = volatilePrice(g.close, prevClose, prevStressedClose, s.volMultiplier, step);
      out.open = o;
      out.close = c;
      out.high = std::max({o, h, l, c});
      out.low = std::min({o, h, l, c});
      out.volumeLots = scaledVolume(orig.volumeLots, s.volumeShare);
      prevClose = g.close;
      prevStressedClose = c;
      if (i + 1 == window && !g.close.isZero()) factorAfter = c.div(g.close, kHalfUp);
    } else {
      out.open = snapPrice(g.open.mul(factorAfter, kHalfUp), step);
      out.high = snapPrice(g.high.mul(factorAfter, kHalfUp), step);
      out.low = snapPrice(g.low.mul(factorAfter, kHalfUp), step);
      out.close = snapPrice(g.close.mul(factorAfter, kHalfUp), step);
    }
  }
}

}  // namespace

Timeline applyStress(const Timeline& timeline, const StressScenario& scenario) {
  validate(scenario);
  Timeline result = timeline;
  const Decimal gapFactor = Decimal::one() - scenario.gapPercent.div(Decimal::fromUnits(100), kHalfUp);
  const int count = static_cast<int>(timeline.instruments().size());
  for (int i = 0; i < count; ++i) stressInstrument(timeline, result, i, scenario, gapFactor);
  return result;
}

}  // namespace core
