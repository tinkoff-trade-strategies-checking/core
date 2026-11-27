#include <cstddef>

#include "core/exec/execution.hpp"

namespace core {

namespace {

Validation invalid(const char *Field) {
  return Validation{false, Reason::InvalidParams, Field};
}

bool positive(const std::optional<Decimal> &V) {
  return V.has_value() && V->sign() > 0;
}

// Цена кратна шагу цены инструмента (`decisions.md`, «Деньги и округление»).
bool onGrid(const Decimal &Price, const Decimal &PriceStep) {
  return PriceStep.sign() > 0 &&
         Price.roundToStep(PriceStep, RoundingMode::FLOOR) == Price;
}

} // namespace

Validation ExecutionEngine::validate(const OrderSpec &Spec,
                                     const Portfolio &Portfolio,
                                     const Timeline &Timeline,
                                     std::int64_t Step) const {
  if (Spec.IsBlindPrice)
    return invalid("isBlindPrice"); // журнал хранит рублёвые цены
  if (Spec.Instrument < 0 ||
      static_cast<std::size_t>(Spec.Instrument) >= Instruments.size()) {
    return invalid("instrument");
  }
  if (Spec.Lots < 1)
    return invalid("lots");

  const bool Buy = Spec.OrderSide == Side::Buy;
  const OrderType Type = Spec.Type;
  if ((Type == OrderType::TakeProfit || Type == OrderType::TrailingStop) && Buy)
    return invalid("side");

  const bool NeedsLimit = Type == OrderType::Limit ||
                          Type == OrderType::StopLimit ||
                          Type == OrderType::TakeProfit;
  if (NeedsLimit && !positive(Spec.LimitPrice))
    return invalid("limitPrice");
  const bool NeedsStop =
      Type == OrderType::Stop || Type == OrderType::StopLimit;
  if (NeedsStop && !positive(Spec.StopPrice))
    return invalid("stopPrice");
  if (Type == OrderType::TrailingStop) {
    const bool Ok = Spec.TrailingPercent && Spec.TrailingPercent->sign() > 0 &&
                    *Spec.TrailingPercent <= Decimal::fromUnits(50);
    if (!Ok)
      return invalid("trailingPercent");
  }

  const Decimal &PriceStep =
      Instruments[static_cast<std::size_t>(Spec.Instrument)].PriceStep;
  if (PriceStep.sign() <= 0)
    return invalid("instrument");
  if (NeedsLimit && !onGrid(*Spec.LimitPrice, PriceStep))
    return invalid("limitPrice");
  if (NeedsStop && !onGrid(*Spec.StopPrice, PriceStep))
    return invalid("stopPrice");

  // Цена для проверок - close последнего бара бумаги не позже шага подачи
  // (`decisions.md`, «Заявки»).
  const StepBar *Last = Timeline.lastBarOf(Spec.Instrument, Step);
  if (Last == nullptr)
    return invalid("instrument"); // бумага ещё не торговалась: цены нет
  const Decimal &Close = Last->Close;

  if (NeedsStop) {
    const bool RightSide =
        Buy ? *Spec.StopPrice > Close : *Spec.StopPrice < Close;
    if (!RightSide)
      return invalid("stopPrice");
  }
  if (Type == OrderType::TakeProfit && !(*Spec.LimitPrice > Close))
    return invalid("limitPrice");

  if (!Margin.isMarginEnabled()) {
    if (Buy) {
      const Instrument &Inst =
          Instruments[static_cast<std::size_t>(Spec.Instrument)];
      try {
        // Округление вверх: проверка не пропускает заявку, на которую не
        // хватает копейки.
        const Decimal Need =
            requiredCashForOrder(Close, Spec.Lots, Inst.LotSize, Costs);
        if (Need > Portfolio.cash())
          return Validation{false, Reason::InsufficientCash, ""};
      } catch (const DecimalOverflow &) {
        return Validation{
            false, Reason::InsufficientCash,
            ""}; // сумма не помещается в Decimal - денег точно не хватает
      }
    } else if (Spec.Lots > Portfolio.lots(Spec.Instrument)) {
      return Validation{false, Reason::InsufficientPosition, ""};
    }
    return Validation{};
  }

  // Маржинальный / шортовый режим (core#82, core#83).
  const std::int64_t CurrentLots = Portfolio.lots(Spec.Instrument);
  const std::int64_t NewLots =
      Buy ? (CurrentLots + Spec.Lots) : (CurrentLots - Spec.Lots);
  if (!Buy && NewLots < 0 && !Margin.AllowShort)
    return Validation{false, Reason::InsufficientPosition, ""};

  // Заявка, исключительно сокращающая текущую позицию без переворота, всегда
  // допустима (улучшает обеспечение).
  const bool ReducesShort = Buy && CurrentLots < 0 && Spec.Lots <= -CurrentLots;
  const bool ReducesLong = !Buy && CurrentLots > 0 && Spec.Lots <= CurrentLots;
  if (ReducesShort || ReducesLong)
    return Validation{};

  const Instrument &Inst =
      Instruments[static_cast<std::size_t>(Spec.Instrument)];
  try {
    Decimal CurrentEquity = Portfolio.cash();
    Decimal OtherGross;
    for (std::size_t I = 0; I < Instruments.size(); ++I) {
      const std::int64_t LotsI = Portfolio.lots(static_cast<int>(I));
      if (LotsI == 0)
        continue;
      const StepBar *BarI = Timeline.lastBarOf(static_cast<int>(I), Step);
      if (BarI == nullptr)
        return Validation{false, Reason::InsufficientMargin, ""};
      const Decimal ValI =
          Portfolio::positionValue(LotsI, Instruments[I].LotSize, BarI->Close);
      CurrentEquity += ValI;
      if (static_cast<int>(I) != Spec.Instrument)
        OtherGross += ValI.abs();
    }
    const Amounts Est = amounts(Close, Spec.Lots, Inst.LotSize, Costs);
    const Decimal ProjectedEquity = CurrentEquity - Est.Commission - Est.Fee;
    const std::int64_t AbsNewLots = NewLots >= 0 ? NewLots : -NewLots;
    const Decimal NewInstrGross =
        Portfolio::positionValue(AbsNewLots, Inst.LotSize, Close);
    const Decimal ProjectedGross = OtherGross + NewInstrGross;
    const Decimal DInit = Margin.effectiveInitialMarginRatio();
    const Decimal RequiredMargin =
        ProjectedGross.mul(DInit, 2, RoundingMode::CEILING);
    if (ProjectedEquity.sign() <= 0 || ProjectedEquity < RequiredMargin)
      return Validation{false, Reason::InsufficientMargin, ""};
  } catch (const DecimalOverflow &) {
    return Validation{false, Reason::InsufficientMargin, ""};
  }
  return Validation{};
}

Validation validateOcoPair(const OrderSpec &First, const OrderSpec &Second) {
  if (First.Instrument != Second.Instrument) {
    return invalid("instrument");
  }
  if (First.OrderSide != Second.OrderSide) {
    return invalid("side");
  }
  // OCO «стоп + тейк»: стоп = стоп-рыночная или стоп-лимитная заявка
  // (решение владельца продукта, `decisions.md`).
  const bool FirstIsStop =
      First.Type == OrderType::Stop || First.Type == OrderType::StopLimit;
  const bool FirstIsTake = First.Type == OrderType::TakeProfit;
  const bool SecondIsStop =
      Second.Type == OrderType::Stop || Second.Type == OrderType::StopLimit;
  const bool SecondIsTake = Second.Type == OrderType::TakeProfit;

  const bool ValidTypes =
      (FirstIsStop && SecondIsTake) || (FirstIsTake && SecondIsStop);
  if (!ValidTypes) {
    return invalid("type");
  }
  // Тейк-профит - только продажа (как в validate), поэтому пара «стоп + тейк»
  // бывает только на продажу.
  if (First.OrderSide != Side::Sell) {
    return invalid("side");
  }
  return Validation{};
}

Decimal requiredCashForOrder(const Decimal &Price, std::int64_t Lots,
                             std::int64_t LotSize, const CostConfig &Costs) {
  const Decimal Factor =
      Decimal::one() + Costs.CommissionRate + Costs.ExchangeFeeRate;
  return Price.times(Lots).times(LotSize).mul(Factor, Decimal::kScale,
                                              RoundingMode::CEILING);
}

} // namespace core
