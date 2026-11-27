#include "core/market/order_book_l2.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>

#include "core/money/rounding.hpp"

namespace core {
namespace {

constexpr std::array<std::int64_t, 6> kWeights = {14, 20, 18, 14, 11, 8};

Decimal resolvePriceStep(const OrderBookConfig &Cfg) {
  if (Cfg.PriceStep < Decimal::zero()) {
    throw std::invalid_argument("PriceStep must be non-negative");
  }
  if (Cfg.PriceStep > Decimal::zero()) {
    return Cfg.PriceStep;
  }
  return Decimal::parse("0.01");
}

Decimal resolveSpreadPct(const OrderBookConfig &Cfg) {
  if (Cfg.SpreadPct < Decimal::zero()) {
    throw std::invalid_argument("SpreadPct must be non-negative");
  }
  if (Cfg.SpreadPct > Decimal::zero()) {
    return Cfg.SpreadPct;
  }
  return Decimal::parse("0.0005");
}

std::int64_t levelWeight(int LevelIndex) {
  const auto Idx = static_cast<std::size_t>(LevelIndex);
  if (Idx < kWeights.size()) {
    return kWeights[Idx];
  }
  return std::max<std::int64_t>(2, 12 - static_cast<std::int64_t>(LevelIndex));
}

Decimal clampPrice(const Decimal &Price, const Decimal &Low,
                   const Decimal &High) {
  if (Price < Low) {
    return Low;
  }
  if (Price > High) {
    return High;
  }
  return Price;
}

Decimal interpolateAnchor(const Decimal &From, const Decimal &To,
                          std::int64_t Offset, std::int64_t Span,
                          const Decimal &Step, const Decimal &Low,
                          const Decimal &High) {
  if (Span <= 0 || Offset <= 0) {
    return clampPrice(From.roundToStep(Step, RoundingMode::HalfUp), Low, High);
  }
  if (Offset >= Span) {
    return clampPrice(To.roundToStep(Step, RoundingMode::HalfUp), Low, High);
  }
  const Decimal Delta = To - From;
  const Decimal Interpolated =
      From + Delta.mulDiv(Decimal::fromUnits(Offset), Decimal::fromUnits(Span),
                          Decimal::kScale, RoundingMode::HalfUp);
  return clampPrice(Interpolated.roundToStep(Step, RoundingMode::HalfUp), Low,
                    High);
}

} // namespace

OrderBookSnapshot buildOrderBook(const Bar &SourceBar,
                                 const OrderBookConfig &Cfg) {
  if (Cfg.Depth <= 0) {
    throw std::invalid_argument("Depth must be positive");
  }
  if (SourceBar.Close <= Decimal::zero()) {
    throw std::invalid_argument("Bar Close must be positive");
  }
  if (SourceBar.Volume < 0) {
    throw std::invalid_argument("Bar Volume must be non-negative");
  }

  const Decimal Step = resolvePriceStep(Cfg);
  const Decimal SpreadPct = resolveSpreadPct(Cfg);
  const Decimal Two = Decimal::fromUnits(2);
  const Decimal HalfSpread =
      SourceBar.Close.mul(SpreadPct, RoundingMode::HalfUp)
          .div(Two, RoundingMode::HalfUp);

  Decimal BestBid =
      (SourceBar.Close - HalfSpread).roundToStep(Step, RoundingMode::FLOOR);
  Decimal BestAsk =
      (SourceBar.Close + HalfSpread).roundToStep(Step, RoundingMode::CEILING);

  if (BestBid <= Decimal::zero()) {
    BestBid = Step;
  }
  if (BestAsk <= BestBid) {
    BestAsk = BestBid + Step;
  }

  std::int64_t TotalWeight = 0;
  for (int I = 0; I < Cfg.Depth; ++I) {
    TotalWeight += levelWeight(I);
  }

  const std::int64_t MinVolume = static_cast<std::int64_t>(Cfg.Depth) * 2;
  const std::int64_t BaseVolume = std::max(MinVolume, SourceBar.Volume);
  std::int64_t BidPct = 50;
  if (SourceBar.Close > SourceBar.Open) {
    BidPct = 55;
  } else if (SourceBar.Close < SourceBar.Open) {
    BidPct = 45;
  }
  const std::int64_t BidBudget =
      std::max<std::int64_t>(Cfg.Depth, (BaseVolume * BidPct) / 100);
  const std::int64_t AskBudget =
      std::max<std::int64_t>(Cfg.Depth, (BaseVolume * (100 - BidPct)) / 100);

  OrderBookSnapshot Snapshot;
  Snapshot.TimeUnixMs = SourceBar.AvailableAtMs > 0 ? SourceBar.AvailableAtMs
                                                    : SourceBar.TsOpenMs;
  Snapshot.MidPrice = (BestBid + BestAsk).div(Two, RoundingMode::HalfUp);
  const Decimal Spread = BestAsk - BestBid;
  const Decimal Bps = Decimal::fromUnits(10000);
  const Decimal Mid = Snapshot.MidPrice;
  Snapshot.SpreadBps =
      Spread.mulDiv(Bps, Mid, Decimal::kScale, RoundingMode::HalfUp);

  Snapshot.Bids.reserve(static_cast<std::size_t>(Cfg.Depth));
  Snapshot.Asks.reserve(static_cast<std::size_t>(Cfg.Depth));

  for (int I = 0; I < Cfg.Depth; ++I) {
    const Decimal Offset = Step.times(I);
    const std::int64_t Weight = levelWeight(I);

    const Decimal BidPrice = BestBid - Offset;
    if (BidPrice > Decimal::zero()) {
      const std::int64_t BidLots =
          std::max<std::int64_t>(1, (BidBudget * Weight) / TotalWeight);
      Snapshot.Bids.push_back(OrderBookLevel{BidPrice, BidLots});
    }

    const Decimal AskPrice = BestAsk + Offset;
    const std::int64_t AskLots =
        std::max<std::int64_t>(1, (AskBudget * Weight) / TotalWeight);
    Snapshot.Asks.push_back(OrderBookLevel{AskPrice, AskLots});
  }

  return Snapshot;
}

std::vector<TapePrint> synthesizeTape(const Bar &SourceBar,
                                      const OrderBookConfig &Cfg) {
  if (Cfg.TapePrintsPerBar <= 0) {
    throw std::invalid_argument("TapePrintsPerBar must be positive");
  }
  if (SourceBar.Volume < 0) {
    throw std::invalid_argument("Bar Volume must be non-negative");
  }
  if (SourceBar.High < SourceBar.Low) {
    throw std::invalid_argument("Bar High must be >= Low");
  }

  const Decimal Step = resolvePriceStep(Cfg);
  if (SourceBar.Volume == 0) {
    return {};
  }

  const std::int64_t Count =
      std::min<std::int64_t>(Cfg.TapePrintsPerBar, SourceBar.Volume);
  const bool Bullish = SourceBar.Close >= SourceBar.Open;
  const Decimal P0 = SourceBar.Open;
  const Decimal P1 = Bullish ? SourceBar.Low : SourceBar.High;
  const Decimal P2 = Bullish ? SourceBar.High : SourceBar.Low;
  const Decimal P3 = SourceBar.Close;

  // Индексы опорных точек траектории Open -> Low/High -> Close.
  std::int64_t Idx1 = 0;
  std::int64_t Idx2 = 0;
  if (Count >= 4) {
    Idx1 = std::max<std::int64_t>(1, (Count - 1) / 3);
    Idx2 = std::min<std::int64_t>(
        Count - 2, std::max<std::int64_t>(Idx1 + 1, (2 * (Count - 1)) / 3));
  }

  // Детерминированное распределение объёма с точной суммой SourceBar.Volume.
  std::vector<std::int64_t> Weights(static_cast<std::size_t>(Count), 2);
  Weights.front() = 3;
  Weights.back() = 3;
  std::int64_t TotalWeight = 0;
  for (std::int64_t W : Weights) {
    TotalWeight += W;
  }

  std::vector<std::int64_t> Lots(static_cast<std::size_t>(Count), 1);
  const std::int64_t Distributable = SourceBar.Volume - Count;
  std::int64_t Assigned = 0;
  for (std::int64_t I = 0; I < Count; ++I) {
    const std::int64_t Share =
        (Distributable * Weights[static_cast<std::size_t>(I)]) / TotalWeight;
    Lots[static_cast<std::size_t>(I)] += Share;
    Assigned += Share;
  }
  const std::int64_t Remainder = Distributable - Assigned;
  for (std::int64_t I = 0; I < Remainder; ++I) {
    ++Lots[static_cast<std::size_t>(I % Count)];
  }

  const std::int64_t StartMs = SourceBar.TsOpenMs;
  const std::int64_t EndMs =
      SourceBar.AvailableAtMs > StartMs ? SourceBar.AvailableAtMs : StartMs;
  const std::int64_t DurationMs = EndMs - StartMs;

  std::vector<TapePrint> Prints;
  Prints.reserve(static_cast<std::size_t>(Count));

  Side PrevSide = Bullish ? Side::Buy : Side::Sell;
  Decimal PrevPrice;

  for (std::int64_t I = 0; I < Count; ++I) {
    Decimal PrintPrice;
    if (Count == 1) {
      PrintPrice = clampPrice(P3.roundToStep(Step, RoundingMode::HalfUp),
                              SourceBar.Low, SourceBar.High);
    } else if (Count == 2) {
      const Decimal Target = (I == 0) ? P0 : P3;
      PrintPrice = clampPrice(Target.roundToStep(Step, RoundingMode::HalfUp),
                              SourceBar.Low, SourceBar.High);
    } else if (Count == 3) {
      const Decimal Target = (I == 0) ? P0 : ((I == 1) ? P2 : P3);
      PrintPrice = clampPrice(Target.roundToStep(Step, RoundingMode::HalfUp),
                              SourceBar.Low, SourceBar.High);
    } else if (I <= Idx1) {
      PrintPrice = interpolateAnchor(P0, P1, I, Idx1, Step, SourceBar.Low,
                                     SourceBar.High);
    } else if (I <= Idx2) {
      PrintPrice = interpolateAnchor(P1, P2, I - Idx1, Idx2 - Idx1, Step,
                                     SourceBar.Low, SourceBar.High);
    } else {
      PrintPrice = interpolateAnchor(P2, P3, I - Idx2, (Count - 1) - Idx2, Step,
                                     SourceBar.Low, SourceBar.High);
    }

    std::int64_t PrintTimeMs = StartMs;
    if (Count == 1) {
      PrintTimeMs = StartMs + DurationMs / 2;
    } else {
      const Int128 ScaledOffset =
          (static_cast<Int128>(DurationMs) * static_cast<Int128>(I)) /
          static_cast<Int128>(Count - 1);
      PrintTimeMs = StartMs + static_cast<std::int64_t>(ScaledOffset);
    }

    Side Aggressor = PrevSide;
    if (I > 0) {
      if (PrintPrice > PrevPrice) {
        Aggressor = Side::Buy;
      } else if (PrintPrice < PrevPrice) {
        Aggressor = Side::Sell;
      }
    }

    Prints.push_back(TapePrint{PrintTimeMs, PrintPrice,
                               Lots[static_cast<std::size_t>(I)], Aggressor});
    PrevPrice = PrintPrice;
    PrevSide = Aggressor;
  }

  return Prints;
}

BookFillResult walkBookFill(const OrderBookSnapshot &Book, Side OrderSide,
                            std::int64_t RequestedLots) {
  if (RequestedLots < 0) {
    throw std::invalid_argument("RequestedLots must be non-negative");
  }
  if (RequestedLots == 0) {
    return BookFillResult{};
  }

  const auto &Levels = (OrderSide == Side::Buy) ? Book.Asks : Book.Bids;

  std::int64_t Remaining = RequestedLots;
  std::int64_t Filled = 0;
  Decimal TotalValue = Decimal::zero();
  Decimal BestPrice = Decimal::zero();
  bool HasBestPrice = false;

  for (const auto &Level : Levels) {
    if (Remaining == 0) {
      break;
    }
    if (Level.Lots <= 0) {
      continue;
    }
    if (!HasBestPrice) {
      BestPrice = Level.Price;
      HasBestPrice = true;
    }
    const std::int64_t Take = std::min(Remaining, Level.Lots);
    TotalValue += Level.Price.times(Take);
    Filled += Take;
    Remaining -= Take;
  }

  if (Filled == 0) {
    BookFillResult Empty;
    Empty.RemainingLots = RequestedLots;
    return Empty;
  }

  const Decimal AvgPrice =
      TotalValue.div(Decimal::fromUnits(Filled), RoundingMode::HalfUp);
  Decimal PriceImpactBps = Decimal::zero();
  if (HasBestPrice && BestPrice > Decimal::zero()) {
    const Decimal Diff = (OrderSide == Side::Buy) ? (AvgPrice - BestPrice)
                                                  : (BestPrice - AvgPrice);
    PriceImpactBps = Diff.mulDiv(Decimal::fromUnits(10000), BestPrice,
                                 Decimal::kScale, RoundingMode::HalfUp);
  }

  return BookFillResult{AvgPrice, Filled, Remaining, PriceImpactBps,
                        TotalValue};
}

} // namespace core
