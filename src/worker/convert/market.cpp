#include "convert/market.hpp"

#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "clickhouse/clickhouse_client.hpp"
#include "core/analysis/stress.hpp"
#include "core/analysis/walk_forward.hpp"
#include "core/money/proto_conv.hpp"

namespace worker {

namespace {

core::Timeframe timeframeFromProto(pb::Timeframe timeframe) {
  switch (timeframe) {
    case pb::S1:
      return core::Timeframe::S1;
    case pb::S10:
      return core::Timeframe::S10;
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

core::Decimal requirePositive(const pb::Decimal& value, const char* what) {
  const core::Decimal d = core::fromProto(value);
  if (d <= core::Decimal::zero()) throw std::invalid_argument(std::string(what) + " должен быть положительным");
  return d;
}

}  // namespace

std::int32_t parseIsoDate(const std::string& text) {
  bool ok = text.size() == 10 && text[4] == '-' && text[7] == '-';
  for (std::size_t i = 0; ok && i < text.size(); ++i) {
    if (i != 4 && i != 7 && !std::isdigit(static_cast<unsigned char>(text[i]))) ok = false;
  }
  if (!ok) throw std::invalid_argument("неверная дата, нужен формат гггг-мм-дд: " + text);
  const std::int32_t year = std::stoi(text.substr(0, 4));
  const std::int32_t month = std::stoi(text.substr(5, 2));
  const std::int32_t day = std::stoi(text.substr(8, 2));
  if (month < 1 || month > 12 || day < 1 || day > 31) throw std::invalid_argument("неверная дата: " + text);
  return year * 10000 + month * 100 + day;
}

std::string formatIsoDate(std::int32_t yyyymmdd) {
  std::string out = std::to_string(yyyymmdd);
  out.insert(6, "-");
  out.insert(4, "-");
  return out;
}

core::SessionSpec toSessionSpec(const pb::MarketSpec& market) {
  if (market.instruments_size() == 0) throw std::invalid_argument("в спецификации нет бумаг");

  core::SessionSpec spec;
  std::set<std::string> keys;
  for (const pb::Instrument& i : market.instruments()) {
    if (i.key().empty() || !keys.insert(i.key()).second) {
      throw std::invalid_argument("ключ бумаги пуст или повторяется: " + i.key());
    }
    if (i.lot_size() <= 0) throw std::invalid_argument("размер лота должен быть положительным: " + i.key());
    spec.instruments.push_back(
        core::Instrument{i.key(), i.ticker(), i.lot_size(), requirePositive(i.price_step(), "шаг цены")});
  }

  spec.naive = market.naive();
  if (market.has_costs()) {
    const pb::CostConfig& c = market.costs();
    spec.costs.commissionRate = core::fromProto(c.commission_rate());
    spec.costs.exchangeFeeRate = core::fromProto(c.exchange_fee_rate());
    spec.costs.spread = core::fromProto(c.spread());
    spec.costs.slippageKappa = core::fromProto(c.slippage_kappa());
    if (c.has_liquidity_share()) spec.costs.liquidityShare = core::fromProto(c.liquidity_share());
  } else if (!spec.naive) {
    throw std::invalid_argument("не заданы издержки исполнения");
  }
  spec.initialCash = requirePositive(market.initial_cash(), "начальный капитал");
  return spec;
}

std::shared_ptr<const core::Timeline> loadTimeline(Context& context, const pb::MarketSpec& market) {
  std::vector<core::Instrument> instruments = toSessionSpec(market).instruments;
  core::Timeline timeline = core::Timeline::load(
      context.bars, context.calendar, market.snapshot_id(), std::move(instruments),
      timeframeFromProto(market.timeframe()), parseIsoDate(market.from_date()), parseIsoDate(market.to_date()));
  if (timeline.totalSteps() == 0) throw core::SnapshotNotFound("в снапшоте нет баров за период");
  return std::make_shared<const core::Timeline>(std::move(timeline));
}

namespace {

std::optional<core::StressScenario> stressOf(const pb::StressScenario& stress, std::int64_t totalSteps) {
  core::StressScenario scenario;
  scenario.gapPercent = core::fromProto(stress.gap_percent());
  scenario.volMultiplier = core::fromProto(stress.vol_multiplier());
  scenario.volBars = stress.vol_bars();
  scenario.volumeShare = core::fromProto(stress.volume_share());
  // Ненулевые отрицательные значения тоже идут в applyStress, он откажет (INVALID_ARGUMENT).
  const bool active = scenario.volBars != 0 || scenario.gapPercent != core::Decimal::zero() ||
                      scenario.volumeShare != core::Decimal::zero() || scenario.volMultiplier < core::Decimal::zero();
  if (!active) return std::nullopt;
  scenario.startStep = stress.start_step() < 0 ? totalSteps / 2 : stress.start_step();
  if (scenario.startStep >= totalSteps) {
    throw std::invalid_argument("стресс: start_step " + std::to_string(scenario.startStep) +
                                " за пределами шкалы времени, шагов " + std::to_string(totalSteps));
  }
  return scenario;
}

}  // namespace

std::shared_ptr<const core::Timeline> loadStressedTimeline(Context& context, const pb::MarketSpec& market,
                                                           const pb::StressScenario& stress, bool hasStress) {
  std::shared_ptr<const core::Timeline> timeline = loadTimeline(context, market);
  if (hasStress) {
    if (const auto scenario = stressOf(stress, timeline->totalSteps())) {
      timeline = std::make_shared<const core::Timeline>(core::applyStress(*timeline, *scenario));
    }
  }
  return timeline;
}

std::optional<std::pair<core::Decimal, core::Decimal>> loadBenchmark(Context& context, const pb::MarketSpec& market) {
  if (market.benchmark_ticker().empty()) return std::nullopt;
  // Однобумажная дневная шкала бенчмарка: из неё берём open первого и close последнего бара.
  // Лот и шаг цены для чтения баров не нужны.
  core::Instrument index{market.benchmark_ticker(), market.benchmark_ticker(), 1, core::Decimal::parse("0.01")};
  try {
    const core::Timeline timeline =
        core::Timeline::load(context.bars, context.calendar, market.snapshot_id(), {index}, core::Timeframe::D1,
                             parseIsoDate(market.from_date()), parseIsoDate(market.to_date()));
    if (timeline.totalSteps() == 0) return std::nullopt;
    return std::make_pair(timeline.barsAt(0).front().open, timeline.barsAt(timeline.totalSteps() - 1).front().close);
  } catch (const core::SecurityNotFound&) {
    return std::nullopt;
  }
}

void toProto(const core::StepBar& bar, const core::Timeline& timeline, pb::Bar* out) {
  out->set_step(bar.step);
  out->set_instrument_key(timeline.instruments().at(static_cast<std::size_t>(bar.instrument)).key);
  out->set_time_unix_ms(bar.timeUnixMs);
  out->set_trading_day(bar.tradingDay);
  core::toProto(bar.open, out->mutable_open());
  core::toProto(bar.high, out->mutable_high());
  core::toProto(bar.low, out->mutable_low());
  core::toProto(bar.close, out->mutable_close());
  out->set_volume_lots(bar.volumeLots);
  out->set_after_halt(bar.afterHalt);
}

// Прочие MarketDataError (повреждённые данные, неожиданные ответы ClickHouse) - INTERNAL без деталей.
grpc::Status statusFromCurrentException() {
  try {
    throw;
  } catch (const ClickHouseError&) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "хранилище данных недоступно");
  } catch (const core::SnapshotNotFound& e) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what());
  } catch (const core::SecurityNotFound& e) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what());
  } catch (const core::NoValidFitRun&) {
    // Доменный исход для стратегии (повтор бессмыслен): общий текст без деталей.
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "walk-forward: ни один набор параметров не дал засчитываемого прогона");
  } catch (const IntervalUnavailable& e) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, e.what());
  } catch (const std::invalid_argument& e) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what());
  } catch (...) {
    return grpc::Status(grpc::StatusCode::INTERNAL, "внутренняя ошибка воркера");
  }
}

}  // namespace worker
