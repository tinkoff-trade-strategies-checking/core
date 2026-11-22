#include "replay.hpp"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/engine/journal.hpp"
#include "core/engine/session_engine.hpp"
#include "core/exec/costs.hpp"
#include "core/exec/order.hpp"
#include "core/market/date_utils.hpp"
#include "core/market/instrument.hpp"
#include "core/market/timeframe.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp"
#include "core/report/benchmark.hpp"
#include "core/report/metrics.hpp"
#include "core/report/metrics_input.hpp"
#include "core/report/ruonia.hpp"
#include "json.hpp"
#include "support/fixture1.hpp"

namespace cli {

namespace {

std::string toUpper(std::string_view Str) {
  std::string Out(Str);
  for (char &Ch : Out)
    Ch = static_cast<char>(std::toupper(static_cast<unsigned char>(Ch)));
  return Out;
}

std::int32_t parseYmd(const JsonValue &Val, std::string_view Context) {
  if (Val.isInt()) {
    const std::int64_t Raw = Val.asInt64(Context);
    if (Raw < 19000101 || Raw > 29991231)
      throw std::runtime_error(std::string(Context) + ": некорректная дата");
    return static_cast<std::int32_t>(Raw);
  }
  const std::string &Str = Val.asString(Context);
  if (Str.size() == 10 && Str[4] == '-' && Str[7] == '-')
    return core::parseIsoDate(Str);
  if (Str.size() == 8)
    return std::stoi(Str);
  throw std::runtime_error(std::string(Context) +
                           ": ожидалась дата YYYY-MM-DD или YYYYMMDD");
}

core::Timeframe parseTimeframe(std::string_view Str) {
  const std::string Upper = toUpper(Str);
  if (Upper == "M1")
    return core::Timeframe::M1;
  if (Upper == "M5")
    return core::Timeframe::M5;
  if (Upper == "M10")
    return core::Timeframe::M10;
  if (Upper == "M15")
    return core::Timeframe::M15;
  if (Upper == "H1")
    return core::Timeframe::H1;
  if (Upper == "D1")
    return core::Timeframe::D1;
  throw std::runtime_error("Неизвестный timeframe: " + std::string(Str));
}

core::Side parseSide(std::string_view Str) {
  const std::string Upper = toUpper(Str);
  if (Upper == "BUY")
    return core::Side::Buy;
  if (Upper == "SELL")
    return core::Side::Sell;
  throw std::runtime_error("Неизвестный side: " + std::string(Str));
}

core::OrderType parseOrderType(std::string_view Str) {
  const std::string Upper = toUpper(Str);
  if (Upper == "MARKET")
    return core::OrderType::Market;
  if (Upper == "LIMIT")
    return core::OrderType::Limit;
  if (Upper == "STOP")
    return core::OrderType::Stop;
  if (Upper == "STOP_LIMIT")
    return core::OrderType::StopLimit;
  if (Upper == "TAKE_PROFIT")
    return core::OrderType::TakeProfit;
  if (Upper == "TRAILING_STOP")
    return core::OrderType::TrailingStop;
  throw std::runtime_error("Неизвестный тип заявки: " + std::string(Str));
}

core::TimeInForce parseTif(std::string_view Str) {
  const std::string Upper = toUpper(Str);
  if (Upper == "DAY")
    return core::TimeInForce::Day;
  if (Upper == "GTC")
    return core::TimeInForce::Gtc;
  throw std::runtime_error("Неизвестный time_in_force: " + std::string(Str));
}

int findInstrumentIndex(const std::vector<core::Instrument> &Instruments,
                        std::string_view Key) {
  for (std::size_t I = 0; I < Instruments.size(); ++I) {
    if (Instruments[I].Key == Key || Instruments[I].Ticker == Key)
      return static_cast<int>(I);
  }
  throw std::runtime_error("Неизвестный инструмент: " + std::string(Key));
}

core::OrderSpec parseOrderSpec(const JsonObject &Obj,
                               const std::vector<core::Instrument> &Instruments,
                               std::string_view Context) {
  core::OrderSpec Spec;
  if (const JsonValue *KeyVal = findField(Obj, "instrument_key")) {
    Spec.Instrument =
        findInstrumentIndex(Instruments, KeyVal->asString(Context));
  } else if (const JsonValue *IdxVal = findField(Obj, "instrument")) {
    Spec.Instrument = static_cast<int>(IdxVal->asInt64(Context));
  } else {
    throw std::runtime_error(std::string(Context) +
                             ": отсутствует instrument_key");
  }
  Spec.OrderSide =
      parseSide(requireField(Obj, "side", Context).asString(Context));
  Spec.Type =
      parseOrderType(requireField(Obj, "type", Context).asString(Context));
  Spec.Lots = requireField(Obj, "lots", Context).asInt64(Context);

  if (const JsonValue *Val = findField(Obj, "limit_price");
      Val && !Val->isNull())
    Spec.LimitPrice = Val->asDecimal("limit_price");
  if (const JsonValue *Val = findField(Obj, "stop_price");
      Val && !Val->isNull())
    Spec.StopPrice = Val->asDecimal("stop_price");
  if (const JsonValue *Val = findField(Obj, "trailing_percent");
      Val && !Val->isNull())
    Spec.TrailingPercent = Val->asDecimal("trailing_percent");
  if (const JsonValue *Val = findField(Obj, "time_in_force");
      Val && !Val->isNull())
    Spec.Tif = parseTif(Val->asString("time_in_force"));
  return Spec;
}

core::PlaceCmd parsePlaceCmd(const JsonObject &Obj,
                             const std::vector<core::Instrument> &Instruments,
                             std::string_view Context) {
  core::PlaceCmd Cmd;
  Cmd.Order =
      parseOrderSpec(requireField(Obj, "order", Context).asObject(Context),
                     Instruments, Context);
  if (const JsonValue *Oco = findField(Obj, "oco_with");
      Oco && !Oco->isNull()) {
    Cmd.OcoWith =
        parseOrderSpec(Oco->asObject("oco_with"), Instruments, "oco_with");
  }
  return Cmd;
}

core::CostConfig parseCosts(const JsonObject &Obj) {
  if (const JsonValue *Zero = findField(Obj, "zero");
      Zero && Zero->isBool() && Zero->asBool()) {
    return core::CostConfig::zero();
  }
  core::CostConfig Costs = core::CostConfig::defaults();
  if (const JsonValue *V = findField(Obj, "commission_rate"))
    Costs.CommissionRate = V->asDecimal("commission_rate");
  if (const JsonValue *V = findField(Obj, "exchange_fee_rate"))
    Costs.ExchangeFeeRate = V->asDecimal("exchange_fee_rate");
  if (const JsonValue *V = findField(Obj, "spread"))
    Costs.Spread = V->asDecimal("spread");
  if (const JsonValue *V = findField(Obj, "slippage_kappa"))
    Costs.SlippageKappa = V->asDecimal("slippage_kappa");
  if (const JsonValue *V = findField(Obj, "liquidity_share")) {
    if (V->isNull())
      Costs.LiquidityShare.reset();
    else
      Costs.LiquidityShare = V->asDecimal("liquidity_share");
  }
  return Costs;
}

std::string statusToString(core::SessionStatus Status) {
  switch (Status) {
  case core::SessionStatus::Running:
    return "RUNNING";
  case core::SessionStatus::FinishedEnd:
    return "FINISHED_END";
  case core::SessionStatus::Bankrupt:
    return "BANKRUPT";
  }
  return "UNKNOWN";
}

std::string sideToString(core::Side OrderSide) {
  return OrderSide == core::Side::Buy ? "BUY" : "SELL";
}

std::string orderStatusToString(core::OrderStatus Status) {
  switch (Status) {
  case core::OrderStatus::Active:
    return "ACTIVE";
  case core::OrderStatus::PartiallyFilled:
    return "PARTIALLY_FILLED";
  case core::OrderStatus::Filled:
    return "FILLED";
  case core::OrderStatus::Cancelled:
    return "CANCELLED";
  case core::OrderStatus::Expired:
    return "EXPIRED";
  case core::OrderStatus::Rejected:
    return "REJECTED";
  }
  return "UNKNOWN";
}

std::string reasonToString(core::Reason ReasonCode) {
  switch (ReasonCode) {
  case core::Reason::None:
    return "NONE";
  case core::Reason::InvalidParams:
    return "INVALID_PARAMS";
  case core::Reason::InsufficientCash:
    return "INSUFFICIENT_CASH";
  case core::Reason::InsufficientPosition:
    return "INSUFFICIENT_POSITION";
  case core::Reason::SessionNotActive:
    return "SESSION_NOT_ACTIVE";
  case core::Reason::OrderNotActive:
    return "ORDER_NOT_ACTIVE";
  case core::Reason::LiquidityLimit:
    return "LIQUIDITY_LIMIT";
  case core::Reason::OcoSiblingFilled:
    return "OCO_SIBLING_FILLED";
  case core::Reason::PositionClosed:
    return "POSITION_CLOSED";
  case core::Reason::Replaced:
    return "REPLACED";
  case core::Reason::SessionEnd:
    return "SESSION_END";
  case core::Reason::CancelledByUser:
    return "CANCELLED_BY_USER";
  case core::Reason::DayExpired:
    return "DAY_EXPIRED";
  }
  return "UNKNOWN";
}

JsonValue optDouble(const std::optional<double> &Val) {
  if (!Val)
    return JsonValue(nullptr);
  return JsonValue(*Val);
}

JsonValue buildReportJson(const core::ReplayResult &Replay,
                          const core::Report &Report,
                          const std::vector<core::Instrument> &Instruments) {
  JsonObject MetricsObj;
  MetricsObj["total_return"] = Report.Stats.TotalReturn;
  MetricsObj["gross_return"] = Report.Stats.GrossReturn;
  MetricsObj["cagr"] = optDouble(Report.Stats.Cagr);
  MetricsObj["max_drawdown"] = Report.Stats.MaxDrawdown;
  MetricsObj["max_drawdown_steps"] = Report.Stats.MaxDrawdownSteps;
  MetricsObj["volatility"] = optDouble(Report.Stats.Volatility);
  MetricsObj["sharpe"] = optDouble(Report.Stats.Sharpe);
  MetricsObj["sortino"] = optDouble(Report.Stats.Sortino);
  MetricsObj["turnover"] = Report.Stats.Turnover;
  MetricsObj["fills"] = Report.Stats.Fills;
  MetricsObj["round_trips"] = Report.Stats.RoundTrips;
  MetricsObj["win_rate"] = optDouble(Report.Stats.WinRate);
  MetricsObj["rejections"] = Report.Stats.Rejections;
  MetricsObj["partial_fills"] = Report.Stats.PartialFills;
  MetricsObj["hhi_avg"] = Report.Stats.HhiAvg;
  MetricsObj["max_position_share"] = Report.Stats.MaxPositionShare;
  MetricsObj["benchmark_return"] = optDouble(Report.Stats.BenchmarkReturn);
  MetricsObj["trading_days"] =
      static_cast<std::int64_t>(Report.Stats.TradingDays);
  MetricsObj["stop_take_same_bar"] = Report.Stats.StopTakeSameBar;

  JsonObject CostsObj;
  CostsObj["commission"] = Report.Costs.Commission.toString();
  CostsObj["fee"] = Report.Costs.Fee.toString();
  CostsObj["spread"] = Report.Costs.Spread.toString();
  CostsObj["slippage"] = Report.Costs.Slippage.toString();

  JsonArray FillsArr;
  for (const core::Fill &F : Replay.Engine.fills()) {
    JsonObject Item;
    Item["order_id"] = F.OrderId;
    Item["step"] = F.Step;
    const auto Idx = static_cast<std::size_t>(F.Instrument);
    Item["instrument_key"] =
        Idx < Instruments.size() ? Instruments[Idx].Key : "";
    Item["secid"] = Idx < Instruments.size() ? Instruments[Idx].Ticker : "";
    Item["side"] = sideToString(F.OrderSide);
    Item["lots"] = F.Lots;
    Item["price"] = F.Price.toString();
    Item["base_price"] = F.BasePrice.toString();
    Item["value"] = F.Value.toString();
    Item["commission"] = F.Commission.toString();
    Item["fee"] = F.Fee.toString();
    Item["spread_cost"] = F.SpreadCost.toString();
    Item["slippage_cost"] = F.SlippageCost.toString();
    Item["stop_and_take_same_bar"] = F.StopAndTakeSameBar;
    FillsArr.emplace_back(std::move(Item));
  }

  JsonArray TripsArr;
  for (const core::RoundTrip &Trip : Report.RoundTrips) {
    JsonObject Item;
    const auto Idx = static_cast<std::size_t>(Trip.Instrument);
    Item["instrument_key"] =
        Idx < Instruments.size() ? Instruments[Idx].Key : "";
    Item["secid"] = Idx < Instruments.size() ? Instruments[Idx].Ticker : "";
    Item["entry_step"] = Trip.EntryStep;
    Item["exit_step"] = Trip.ExitStep;
    Item["lots"] = Trip.Lots;
    Item["entry_price"] = Trip.EntryPrice.toString();
    Item["exit_price"] = Trip.ExitPrice.toString();
    Item["gross_pnl"] = (Trip.ExitValue - Trip.EntryValue).toString();
    Item["costs"] = Trip.Costs.toString();
    Item["net_pnl"] = Trip.NetPnl.toString();
    TripsArr.emplace_back(std::move(Item));
  }

  JsonArray OrdersArr;
  for (const core::Order &O : Replay.Engine.orders()) {
    JsonObject Item;
    Item["id"] = O.Id;
    const auto Idx = static_cast<std::size_t>(O.Spec.Instrument);
    Item["instrument_key"] =
        Idx < Instruments.size() ? Instruments[Idx].Key : "";
    Item["secid"] = Idx < Instruments.size() ? Instruments[Idx].Ticker : "";
    Item["side"] = sideToString(O.Spec.OrderSide);
    Item["lots"] = O.Spec.Lots;
    Item["filled_lots"] = O.FilledLots;
    Item["status"] = orderStatusToString(O.Status);
    Item["reason"] = reasonToString(O.ReasonCode);
    Item["placed_step"] = O.PlacedStep;
    Item["closed_step"] = O.ClosedStep;
    OrdersArr.emplace_back(std::move(Item));
  }

  JsonArray CmdArr;
  for (const core::CommandResult &Res : Replay.CommandResults) {
    JsonObject Item;
    Item["seq"] = Res.Seq;
    Item["accepted"] = Res.Accepted;
    Item["reason"] = reasonToString(Res.ReasonCode);
    Item["field"] = Res.Field;
    JsonArray Ids;
    for (const std::int64_t Id : Res.OrderIds)
      Ids.emplace_back(Id);
    Item["order_ids"] = std::move(Ids);
    CmdArr.emplace_back(std::move(Item));
  }

  const core::PortfolioSnapshot Snap = Replay.Engine.snapshot();
  JsonArray PosArr;
  for (std::size_t I = 0; I < Instruments.size(); ++I) {
    JsonObject Pos;
    Pos["instrument_key"] = Instruments[I].Key;
    Pos["secid"] = Instruments[I].Ticker;
    Pos["lots"] = I < Snap.Lots.size() ? Snap.Lots[I] : 0;
    Pos["avg_price"] = I < Snap.AvgPrice.size()
                           ? Snap.AvgPrice[I].toString()
                           : core::Decimal::zero().toString();
    Pos["market_value"] = I < Snap.MarketValue.size()
                              ? Snap.MarketValue[I].toString()
                              : core::Decimal::zero().toString();
    PosArr.emplace_back(std::move(Pos));
  }

  JsonObject PortfolioObj;
  PortfolioObj["cash"] = Snap.Cash.toString();
  PortfolioObj["equity"] = Snap.Equity.toString();
  PortfolioObj["positions"] = std::move(PosArr);

  JsonObject Root;
  Root["status"] = statusToString(Replay.Status);
  Root["last_step"] = Replay.LastStep;
  Root["final_equity"] = Report.FinalEquity.toString();
  Root["metrics"] = std::move(MetricsObj);
  Root["costs"] = std::move(CostsObj);
  Root["fills"] = std::move(FillsArr);
  Root["round_trips"] = std::move(TripsArr);
  Root["orders"] = std::move(OrdersArr);
  Root["command_results"] = std::move(CmdArr);
  Root["portfolio"] = std::move(PortfolioObj);
  return JsonValue(std::move(Root));
}

// Бенчмарк для отчёта CLI: читает D1-ряд по benchmark_ticker за период сессии
// и обрезает по последнему полностью пройденному дню (INV-1). Если тикер не
// задан, отсутствует в снапшоте или нет завершённого дня — nullopt (в JSON
// выводится benchmark_return: null).
std::optional<core::BenchmarkSeries>
loadBenchmarkUpTo(core::testing::Fixture &Fix, const std::string &SnapshotId,
                  const std::string &BenchmarkTicker, std::int32_t FromYmd,
                  std::int32_t ToYmd, const core::Timeline &Timeline,
                  std::int64_t LastStep) {
  if (BenchmarkTicker.empty())
    return std::nullopt;
  const std::optional<std::int32_t> CompletedDay =
      core::lastCompletedDayYmd(Timeline, LastStep);
  if (!CompletedDay)
    return std::nullopt;
  core::Instrument Index{BenchmarkTicker, BenchmarkTicker, 1,
                         core::Decimal::parse("0.01")};
  try {
    const core::Timeline IndexTl =
        core::Timeline::load(Fix.Bars, Fix.Calendar, SnapshotId, {Index},
                             core::Timeframe::D1, FromYmd, ToYmd);
    if (IndexTl.totalSteps() == 0)
      return std::nullopt;
    core::BenchmarkSeries Series;
    Series.FirstOpen = IndexTl.barsAt(0).front().Open;
    for (std::int64_t Step = 0; Step < IndexTl.totalSteps(); ++Step) {
      const std::int32_t Day = IndexTl.yyyymmddOf(Step);
      if (Day > *CompletedDay)
        break;
      Series.Closes.push_back(
          core::DailyClose{Day, IndexTl.barsAt(Step).front().Close});
    }
    if (Series.Closes.empty())
      return std::nullopt;
    return Series;
  } catch (const core::SecurityNotFound &) {
    return std::nullopt;
  }
}

int runBuiltInFixture(std::ostream &Out) {
  core::testing::Fixture Fix = core::testing::fixture1();
  constexpr std::int32_t kFromYmd = 20240109;
  constexpr std::int32_t kToYmd = 20240122;
  auto Tl = std::make_shared<core::Timeline>(core::Timeline::load(
      Fix.Bars, Fix.Calendar, Fix.SnapshotId, Fix.Instruments,
      core::Timeframe::D1, kFromYmd, kToYmd));

  core::SessionSpec Spec;
  Spec.Instruments = Fix.Instruments;
  Spec.Costs = core::CostConfig::defaults();
  Spec.InitialCash = core::Decimal::parse("100000");

  const auto MakeMarketEntry = [](std::int64_t Seq, std::int64_t Step,
                                  core::Side OrderSide) {
    core::OrderSpec Order;
    Order.Instrument = 0;
    Order.OrderSide = OrderSide;
    Order.Type = core::OrderType::Market;
    Order.Lots = 5;
    Order.Tif = core::TimeInForce::Gtc;
    core::JournalEntry Entry;
    Entry.Seq = Seq;
    Entry.Step = Step;
    Entry.Command = core::PlaceCmd{Order, std::nullopt};
    return Entry;
  };
  std::vector<core::JournalEntry> Journal = {
      MakeMarketEntry(0, 0, core::Side::Buy),
      MakeMarketEntry(1, 3, core::Side::Sell)};

  const std::int64_t UpTo = Tl->totalSteps() - 1;
  const core::ReplayResult Replay =
      core::replaySession(Spec, Tl, Journal, UpTo, 0);
  const std::int32_t UpToYmd = Tl->yyyymmddOf(Replay.LastStep);
  const core::RuoniaSeries Ruonia{
      Fix.Ruonia.ruoniaRates(Fix.SnapshotId, UpToYmd)};
  const std::int32_t PrevDay =
      core::previousTradingDay(Fix.Calendar, Fix.SnapshotId, *Tl);
  // Во встроенной фикстуре fixture-1 нет индекса IMOEX — benchmark_return null.
  const core::MetricsInput Input = core::buildMetricsInput(
      Replay.Engine, Spec.InitialCash, *Tl, std::nullopt, Ruonia, PrevDay);
  const core::Report Report = core::computeReport(Input);
  Out << dumpJson(buildReportJson(Replay, Report, Spec.Instruments)) << "\n";
  return 0;
}

std::string readInputText(std::string_view PathArg) {
  if (PathArg == "-") {
    return std::string(std::istreambuf_iterator<char>(std::cin),
                       std::istreambuf_iterator<char>());
  }
  std::ifstream File{std::string(PathArg)};
  if (!File)
    throw std::runtime_error("Не удалось открыть файл: " +
                             std::string(PathArg));
  return std::string(std::istreambuf_iterator<char>(File),
                     std::istreambuf_iterator<char>());
}

int runFromJson(std::string_view PathArg, std::ostream &Out) {
  const JsonValue RootVal = parseJson(readInputText(PathArg));
  const JsonObject &Root = RootVal.asObject("корень");
  const JsonObject &Market =
      requireField(Root, "market", "market").asObject("market");

  const std::string &SnapshotId =
      requireField(Market, "snapshot_id", "market").asString("snapshot_id");
  if (SnapshotId != "fixture-1") {
    throw std::runtime_error("Неизвестный snapshot_id '" + SnapshotId +
                             "' (встроенный: fixture-1)");
  }

  core::testing::Fixture Fix = core::testing::fixture1();
  const core::Timeframe Tf = parseTimeframe(
      requireField(Market, "timeframe", "market").asString("timeframe"));
  const std::int32_t FromYmd =
      parseYmd(requireField(Market, "from_date", "market"), "from_date");
  const std::int32_t ToYmd =
      parseYmd(requireField(Market, "to_date", "market"), "to_date");
  const core::Decimal InitialCash =
      requireField(Market, "initial_cash", "market").asDecimal("initial_cash");

  std::int64_t WarmupBars = 0;
  if (const JsonValue *W = findField(Market, "warmup_bars"); W && !W->isNull())
    WarmupBars = W->asInt64("warmup_bars");

  std::string BenchmarkTicker;
  if (const JsonValue *Bench = findField(Market, "benchmark_ticker");
      Bench && !Bench->isNull()) {
    BenchmarkTicker = Bench->asString("benchmark_ticker");
  }

  core::SessionSpec Spec;
  Spec.Instruments = Fix.Instruments;
  Spec.InitialCash = InitialCash;
  if (const JsonValue *CostsVal = findField(Market, "costs");
      CostsVal && !CostsVal->isNull()) {
    Spec.Costs = parseCosts(CostsVal->asObject("costs"));
  } else {
    Spec.Costs = core::CostConfig::defaults();
  }

  auto Tl = std::make_shared<core::Timeline>(
      WarmupBars > 0
          ? core::Timeline::loadWithWarmup(Fix.Bars, Fix.Calendar, SnapshotId,
                                           Spec.Instruments, Tf, FromYmd, ToYmd,
                                           WarmupBars)
          : core::Timeline::load(Fix.Bars, Fix.Calendar, SnapshotId,
                                 Spec.Instruments, Tf, FromYmd, ToYmd));

  std::vector<core::JournalEntry> Journal;
  if (const JsonValue *JVal = findField(Root, "journal");
      JVal && !JVal->isNull()) {
    const JsonArray &Arr = JVal->asArray("journal");
    Journal.reserve(Arr.size());
    for (std::size_t I = 0; I < Arr.size(); ++I) {
      const std::string Ctx = "journal[" + std::to_string(I) + "]";
      const JsonObject &Item = Arr[I].asObject(Ctx);
      core::JournalEntry Entry;
      Entry.Seq = requireField(Item, "seq", Ctx).asInt64(Ctx);
      Entry.Step = requireField(Item, "step", Ctx).asInt64(Ctx);
      if (const JsonValue *Place = findField(Item, "place")) {
        Entry.Command =
            parsePlaceCmd(Place->asObject(Ctx), Spec.Instruments, Ctx);
      } else if (const JsonValue *Cancel = findField(Item, "cancel")) {
        const JsonObject &CancelObj = Cancel->asObject(Ctx);
        core::CancelCmd Cmd;
        Cmd.OrderId = requireField(CancelObj, "order_id", Ctx).asInt64(Ctx);
        Entry.Command = Cmd;
      } else if (const JsonValue *Replace = findField(Item, "replace")) {
        const JsonObject &RepObj = Replace->asObject(Ctx);
        core::ReplaceCmd Cmd;
        Cmd.OrderId = requireField(RepObj, "order_id", Ctx).asInt64(Ctx);
        Cmd.Replacement = parsePlaceCmd(
            requireField(RepObj, "replacement", Ctx).asObject(Ctx),
            Spec.Instruments, Ctx);
        Entry.Command = std::move(Cmd);
      } else {
        throw std::runtime_error(Ctx +
                                 ": ожидалось поле place, cancel или replace");
      }
      Journal.push_back(std::move(Entry));
    }
  }

  std::int64_t UpToStep = Tl->totalSteps() - 1;
  if (const JsonValue *UpTo = findField(Root, "up_to_step");
      UpTo && !UpTo->isNull()) {
    UpToStep = UpTo->asInt64("up_to_step");
  }

  const core::ReplayResult Replay =
      core::replaySession(Spec, Tl, Journal, UpToStep, 0);
  const std::int32_t UpToYmd = Tl->yyyymmddOf(Replay.LastStep);
  const core::RuoniaSeries Ruonia{
      Fix.Ruonia.ruoniaRates(Fix.SnapshotId, UpToYmd)};
  const std::int32_t PrevDay =
      core::previousTradingDay(Fix.Calendar, SnapshotId, *Tl);
  const std::optional<core::BenchmarkSeries> Benchmark = loadBenchmarkUpTo(
      Fix, SnapshotId, BenchmarkTicker, FromYmd, ToYmd, *Tl, Replay.LastStep);
  const core::MetricsInput Input = core::buildMetricsInput(
      Replay.Engine, Spec.InitialCash, *Tl, Benchmark, Ruonia, PrevDay);
  const core::Report Report = core::computeReport(Input);
  Out << dumpJson(buildReportJson(Replay, Report, Spec.Instruments)) << "\n";
  return 0;
}

} // namespace

void printReplayUsage(std::ostream &Out) {
  Out << "Использование:\n"
         "  sim_cli replay --fixture     прогон встроенной фикстуры fixture-1\n"
         "  sim_cli replay <файл.json>   прогон сценария из JSON-файла ('-' "
         "для stdin)\n";
}

int runReplay(int Argc, char **Argv, std::ostream &Out, std::ostream &Err) {
  if (Argc < 1) {
    printReplayUsage(Err);
    return 2;
  }
  const std::string_view Arg = Argv[0];
  if (Arg == "--help" || Arg == "-h") {
    printReplayUsage(Out);
    return 0;
  }
  if (Argc > 1) {
    printReplayUsage(Err);
    return 2;
  }
  if (Arg == "--fixture")
    return runBuiltInFixture(Out);
  return runFromJson(Arg, Out);
}

} // namespace cli
