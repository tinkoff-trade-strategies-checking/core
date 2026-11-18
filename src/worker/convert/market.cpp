#include "convert/market.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "clickhouse/clickhouse_client.hpp"
#include "core/money/proto_conv.hpp"
#include "core/report/ruonia.hpp"
#include "timeline_cache.hpp"

namespace worker {

namespace {

core::Timeframe timeframeFromProto(pb::Timeframe Timeframe) {
  switch (Timeframe) {
  case pb::M1:
    return core::Timeframe::M1;
  case pb::M5:
    return core::Timeframe::M5;
  case pb::M10:
    return core::Timeframe::M10;
  case pb::M15:
    return core::Timeframe::M15;
  case pb::H1:
    return core::Timeframe::H1;
  case pb::D1:
    return core::Timeframe::D1;
  default:
    throw std::invalid_argument("не указан таймфрейм");
  }
}

core::Decimal requirePositive(const pb::Decimal &Value, const char *What) {
  const core::Decimal D = core::fromProto(Value);
  if (D <= core::Decimal::zero())
    throw std::invalid_argument(std::string(What) +
                                " должен быть положительным");
  return D;
}

} // namespace

std::int32_t parseIsoDate(const std::string &Text) {
  bool Ok = Text.size() == 10 && Text[4] == '-' && Text[7] == '-';
  for (std::size_t I = 0; Ok && I < Text.size(); ++I) {
    if (I != 4 && I != 7 && !std::isdigit(static_cast<unsigned char>(Text[I])))
      Ok = false;
  }
  if (!Ok)
    throw std::invalid_argument("неверная дата, нужен формат гггг-мм-дд: " +
                                Text);
  const std::int32_t Year = std::stoi(Text.substr(0, 4));
  const std::int32_t Month = std::stoi(Text.substr(5, 2));
  const std::int32_t Day = std::stoi(Text.substr(8, 2));
  if (Month < 1 || Month > 12 || Day < 1 || Day > 31)
    throw std::invalid_argument("неверная дата: " + Text);
  return Year * 10000 + Month * 100 + Day;
}

std::string formatIsoDate(std::int32_t Yyyymmdd) {
  std::string Out = std::to_string(Yyyymmdd);
  Out.insert(6, "-");
  Out.insert(4, "-");
  return Out;
}

core::SessionSpec toSessionSpec(const pb::MarketSpec &Market) {
  if (Market.instruments_size() == 0)
    throw std::invalid_argument("в спецификации нет бумаг");

  core::SessionSpec Spec;
  std::set<std::string> Keys;
  for (const pb::Instrument &I : Market.instruments()) {
    if (I.key().empty() || !Keys.insert(I.key()).second) {
      throw std::invalid_argument("ключ бумаги пуст или повторяется: " +
                                  I.key());
    }
    if (I.lot_size() <= 0)
      throw std::invalid_argument("размер лота должен быть положительным: " +
                                  I.key());
    Spec.Instruments.push_back(
        core::Instrument{I.key(), I.ticker(), I.lot_size(),
                         requirePositive(I.price_step(), "шаг цены")});
  }

  if (Market.has_costs()) {
    const pb::CostConfig &C = Market.costs();
    Spec.Costs.CommissionRate = core::fromProto(C.commission_rate());
    Spec.Costs.ExchangeFeeRate = core::fromProto(C.exchange_fee_rate());
    Spec.Costs.Spread = core::fromProto(C.spread());
    Spec.Costs.SlippageKappa = core::fromProto(C.slippage_kappa());
    if (C.has_liquidity_share())
      Spec.Costs.LiquidityShare = core::fromProto(C.liquidity_share());
  } else {
    throw std::invalid_argument("не заданы издержки исполнения");
  }
  Spec.InitialCash =
      requirePositive(Market.initial_cash(), "начальный капитал");
  return Spec;
}

std::shared_ptr<const core::Timeline>
loadTimeline(Context &Context, const pb::MarketSpec &Market) {
  std::vector<core::Instrument> Instruments = toSessionSpec(Market).Instruments;
  if (Market.warmup_bars() < 0 || Market.warmup_bars() > kMaxWarmupBars) {
    throw std::invalid_argument("warmup_bars вне диапазона 0…" +
                                std::to_string(kMaxWarmupBars) + ": " +
                                std::to_string(Market.warmup_bars()));
  }

  const TimelineKey Key{
      .SnapshotId = Market.snapshot_id(),
      .Instruments = Instruments,
      .Timeframe = timeframeFromProto(Market.timeframe()),
      .FromYmd = parseIsoDate(Market.from_date()),
      .ToYmd = parseIsoDate(Market.to_date()),
      .WarmupBars = Market.warmup_bars(),
  };

  if (Context.Cache != nullptr) {
    if (auto Cached = Context.Cache->get(Key)) {
      return Cached;
    }
  }

  for (const core::Instrument &Inst : Instruments) {
    const core::Security Sec =
        Context.Bars.security(Market.snapshot_id(), Inst.Ticker);
    if (Sec.LotSize != Inst.LotSize || Sec.PriceStep != Inst.PriceStep) {
      throw core::MarketDataError("параметры инструмента " + Inst.Ticker +
                                  " не совпадают со снапшотом (securities)");
    }
  }

  core::Timeline Timeline = core::Timeline::loadWithWarmup(
      Context.Bars, Context.Calendar, Key.SnapshotId, std::move(Instruments),
      Key.Timeframe, Key.FromYmd, Key.ToYmd, Key.WarmupBars);
  if (Timeline.totalSteps() == 0)
    throw core::SnapshotNotFound("в снапшоте нет баров за период");
  auto Ptr = std::make_shared<const core::Timeline>(std::move(Timeline));

  if (Context.Cache != nullptr) {
    Context.Cache->put(Key, Ptr);
  }
  return Ptr;
}

std::optional<core::BenchmarkSeries> loadBenchmark(Context &Context,
                                                   const pb::MarketSpec &Market,
                                                   std::int32_t UpToYmd) {
  if (Market.benchmark_ticker().empty())
    return std::nullopt;
  // Однобумажная дневная шкала бенчмарка: из неё берём open первого бара и
  // close каждого дня. Лот и шаг цены для чтения баров не нужны.
  core::Instrument Index{Market.benchmark_ticker(), Market.benchmark_ticker(),
                         1, core::Decimal::parse("0.01")};
  const TimelineKey Key{
      .SnapshotId = Market.snapshot_id(),
      .Instruments = {Index},
      .Timeframe = core::Timeframe::D1,
      .FromYmd = parseIsoDate(Market.from_date()),
      .ToYmd = parseIsoDate(Market.to_date()),
      .WarmupBars = 0,
  };
  try {
    std::shared_ptr<const core::Timeline> Ptr;
    if (Context.Cache != nullptr) {
      Ptr = Context.Cache->get(Key);
    }
    if (!Ptr) {
      core::Timeline Timeline = core::Timeline::load(
          Context.Bars, Context.Calendar, Key.SnapshotId, {Index},
          core::Timeframe::D1, Key.FromYmd, Key.ToYmd);
      if (Timeline.totalSteps() == 0)
        return std::nullopt;
      Ptr = std::make_shared<const core::Timeline>(std::move(Timeline));
      if (Context.Cache != nullptr) {
        Context.Cache->put(Key, Ptr);
      }
    }
    core::BenchmarkSeries Series;
    Series.FirstOpen = Ptr->barsAt(0).front().Open;
    for (std::int64_t Step = 0; Step < Ptr->totalSteps(); ++Step) {
      const std::int32_t Day = Ptr->yyyymmddOf(Step);
      if (Day > UpToYmd)
        break;
      Series.Closes.push_back(
          core::DailyClose{Day, Ptr->barsAt(Step).front().Close});
    }
    if (Series.Closes.empty())
      return std::nullopt;
    return Series;
  } catch (const core::SecurityNotFound &) {
    return std::nullopt;
  }
}

std::int32_t previousTradingDay(Context &Context, const pb::MarketSpec &Market,
                                const core::Timeline &Timeline) {
  const std::int32_t First = Timeline.yyyymmddOf(0);
  const std::int32_t To = core::addCalendarDays(First, -1);
  for (std::int32_t Lookback = 31; Lookback <= 400; Lookback *= 2) {
    const std::vector<core::TradingDay> Days = Context.Calendar.tradingDays(
        Market.snapshot_id(), core::addCalendarDays(First, -Lookback), To);
    for (auto It = Days.rbegin(); It != Days.rend(); ++It) {
      if (It->IsTrading)
        return It->Yyyymmdd;
    }
  }
  const std::int32_t Warmup = Timeline.previousDayYmd();
  return Warmup < First ? Warmup : 0;
}

core::RuoniaSeries loadRuonia(Context &Context, const pb::MarketSpec &Market,
                              std::int32_t UpToYmd) {
  return core::RuoniaSeries{
      Context.Ruonia.ruoniaRates(Market.snapshot_id(), UpToYmd)};
}

void toProto(const core::StepBar &Bar, const core::Timeline &Timeline,
             pb::Bar *Out) {
  Out->set_step(Bar.Step);
  Out->set_instrument_key(
      Timeline.instruments().at(static_cast<std::size_t>(Bar.Instrument)).Key);
  Out->set_time_unix_ms(Bar.TimeUnixMs);
  Out->set_trading_day(Bar.TradingDay);
  core::toProto(Bar.Open, Out->mutable_open());
  core::toProto(Bar.High, Out->mutable_high());
  core::toProto(Bar.Low, Out->mutable_low());
  core::toProto(Bar.Close, Out->mutable_close());
  Out->set_volume_lots(Bar.VolumeLots);
  Out->set_after_halt(Bar.AfterHalt);
}

void appendBars(const core::Timeline &Timeline, std::int64_t FromStep,
                std::int64_t ToStep,
                google::protobuf::RepeatedPtrField<pb::Bar> *Out) {
  const std::int64_t Total = Timeline.totalSteps();
  if (FromStep < -Timeline.warmupSteps() || ToStep >= Total ||
      FromStep > ToStep) {
    throw std::invalid_argument("диапазон шагов [" + std::to_string(FromStep) +
                                "; " + std::to_string(ToStep) +
                                "] вне шкалы времени, шагов " +
                                std::to_string(Total) + ", прогрева " +
                                std::to_string(Timeline.warmupSteps()));
  }
  for (const core::StepBar &B : Timeline.warmupBars()) {
    if (B.Step >= FromStep && B.Step <= ToStep)
      toProto(B, Timeline, Out->Add());
  }
  for (std::int64_t Step = std::max<std::int64_t>(FromStep, 0); Step <= ToStep;
       ++Step) {
    for (const core::StepBar &B : Timeline.barsAt(Step))
      toProto(B, Timeline, Out->Add());
  }
}

// Прочие MarketDataError (повреждённые данные, неожиданные ответы ClickHouse) -
// INTERNAL без деталей.
grpc::Status statusFromCurrentException() {
  try {
    throw;
  } catch (const ClickHouseError &) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "хранилище данных недоступно");
  } catch (const core::SnapshotNotFound &E) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, E.what());
  } catch (const core::SecurityNotFound &E) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, E.what());
  } catch (const IntervalUnavailable &E) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, E.what());
  } catch (const std::invalid_argument &E) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, E.what());
  } catch (const core::DecimalOverflow &E) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, E.what());
  } catch (...) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "внутренняя ошибка воркера");
  }
}

} // namespace worker
