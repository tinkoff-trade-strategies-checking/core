#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "convert/market.hpp"
#include "convert/run.hpp"
#include "core/analysis/walk_forward.hpp"
#include "core/dsl/checker.hpp"
#include "core/dsl/parser.hpp"
#include "handlers.hpp"

namespace worker {

namespace {

// Клиент закрыл поток: прогресс не уходит, считать дальше незачем.
struct StreamClosed {};

// Не чаще одного сообщения на 0,1 % работы: прогонов подбора много, а шагов в каждом - до сотен тысяч.
constexpr std::int64_t kProgressSlices = 1000;

// Значения из запроса: 0 (поле не задано в proto3) - значение по умолчанию ядра; отрицательное - отказ.
std::int32_t orDefault(std::int32_t value, std::int32_t fallback, const char* name) {
  if (value < 0) throw std::invalid_argument(std::string("walk-forward: ") + name + " не может быть отрицательным");
  return value == 0 ? fallback : value;
}

core::WalkForwardConfig configOf(const pb::WalkForwardRequest& request) {
  const core::WalkForwardConfig defaults;
  core::WalkForwardConfig config;
  config.fitDays = orDefault(request.fit_days(), defaults.fitDays, "fit_days");
  config.testDays = orDefault(request.test_days(), defaults.testDays, "test_days");
  config.shiftDays = orDefault(request.shift_days(), defaults.shiftDays, "shift_days");
  config.maxGrid = orDefault(request.max_grid(), defaults.maxGrid, "max_grid");
  return config;
}

// Ошибки DSL - INVALID_ARGUMENT, коды диагностик через ';' (контракт simulation.proto).
std::string errorCodes(const std::vector<core::dsl::Diagnostic>& diagnostics) {
  std::string codes;
  for (const core::dsl::Diagnostic& d : diagnostics) {
    if (d.level != core::dsl::Level::Error) continue;
    if (!codes.empty()) codes += ';';
    codes += d.code;
  }
  return codes;
}

struct Strategy {
  core::dsl::Program program;
  std::vector<core::dsl::ResolvedParam> params;
};

Strategy loadStrategy(const std::string& source) {
  core::dsl::ParseResult parsed = core::dsl::parse(source);
  if (!parsed.program) throw std::invalid_argument("стратегия не разобрана: " + errorCodes(parsed.diagnostics));
  core::dsl::CheckResult checked = core::dsl::check(*parsed.program);
  if (checked.level == core::dsl::Level::Error) {
    throw std::invalid_argument("стратегия не проходит проверку: " + errorCodes(checked.diagnostics));
  }
  return Strategy{std::move(*parsed.program), std::move(checked.params)};
}

// Московская дата каждого торгового дня шкалы (индекс - номер дня с 1).
std::vector<std::int32_t> datesOfDays(const core::Timeline& timeline) {
  std::vector<std::int32_t> dates(static_cast<std::size_t>(timeline.tradingDays()) + 1, 0);
  for (std::int64_t step = 0; step < timeline.totalSteps(); ++step) {
    dates[static_cast<std::size_t>(timeline.tradingDayOf(step))] = timeline.yyyymmddOf(step);
  }
  return dates;
}

void fillResult(const core::WalkForwardResult& result, const Strategy& strategy, const core::Timeline& timeline,
                pb::WalkForwardResult* out) {
  const std::vector<std::int32_t> dates = datesOfDays(timeline);
  const auto dateOf = [&](std::int32_t day) { return formatIsoDate(dates[static_cast<std::size_t>(day)]); };
  for (const core::WalkForwardSegment& s : result.segments) {
    pb::WalkForwardSegment* segment = out->add_segments();
    segment->set_index(s.index);
    segment->set_fit_from(dateOf(s.fitFromDay));
    segment->set_fit_to(dateOf(s.fitToDay));
    segment->set_test_from(dateOf(s.testFromDay));
    segment->set_test_to(dateOf(s.testToDay));
    for (const core::dsl::ResolvedParam& p : strategy.params) {  // порядок объявления, не алфавитный
      pb::ParamValue* value = segment->add_chosen();
      value->set_name(p.name);
      value->set_value(s.chosen.at(p.name));
    }
    segment->set_fit_net_return(s.fitNetReturn);
    // Шаги и сделки проверки - в нумерации среза шкалы: перевод идёт по нему.
    const core::Timeline testTimeline = timeline.slice(s.testFromDay, s.testToDay);
    toProto(s.test, s.testReport, std::nullopt, testTimeline, segment->mutable_test_result());
  }
  out->set_chained_return(result.chainedReturn);
}

core::Progress progressWriter(grpc::ServerWriterInterface<pb::WalkForwardUpdate>* writer) {
  auto lastSlice = std::make_shared<std::int64_t>(-1);
  return [writer, lastSlice](std::int64_t done, std::int64_t total) {
    const std::int64_t slice = total > 0 ? done * kProgressSlices / total : kProgressSlices;
    if (slice == *lastSlice) return;
    *lastSlice = slice;
    pb::WalkForwardUpdate update;
    update.mutable_progress()->set_done_steps(done);
    update.mutable_progress()->set_total_steps(total);
    if (!writer->Write(update)) throw StreamClosed{};
  };
}

}  // namespace

// Ни одно исключение не выходит наружу (statusFromCurrentException). Результат собирается целиком и пишется в поток
// последним сообщением; до него идёт только прогресс.
grpc::Status handleRunWalkForward(Context& context, const pb::WalkForwardRequest& request,
                                  grpc::ServerWriterInterface<pb::WalkForwardUpdate>* writer) {
  try {
    const core::SessionSpec spec = toSessionSpec(request.market());
    const core::WalkForwardConfig config = configOf(request);
    const Strategy strategy = loadStrategy(request.source());
    const std::shared_ptr<const core::Timeline> timeline = loadTimeline(context, request.market());

    const core::WalkForwardResult result =
        core::walkForward(spec, timeline, strategy.program, strategy.params, config, progressWriter(writer));

    pb::WalkForwardUpdate update;
    fillResult(result, strategy, *timeline, update.mutable_result());
    if (!writer->Write(update)) return grpc::Status(grpc::StatusCode::CANCELLED, "клиент закрыл поток");
    return grpc::Status::OK;
  } catch (const StreamClosed&) {
    return grpc::Status(grpc::StatusCode::CANCELLED, "клиент закрыл поток");
  } catch (...) {
    return statusFromCurrentException();
  }
}

}  // namespace worker
