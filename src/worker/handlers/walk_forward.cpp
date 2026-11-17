#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "convert/market.hpp"
#include "convert/progress.hpp"
#include "convert/run.hpp"
#include "convert/strategy.hpp"
#include "core/analysis/walk_forward.hpp"
#include "handlers.hpp"

namespace worker {

namespace {

// Значения из запроса: 0 (поле не задано в proto3) - значение по умолчанию
// ядра; отрицательное - отказ.
std::int32_t orDefault(std::int32_t Value, std::int32_t Fallback,
                       const char *Name) {
  if (Value < 0)
    throw std::invalid_argument(std::string("walk-forward: ") + Name +
                                " не может быть отрицательным");
  return Value == 0 ? Fallback : Value;
}

core::WalkForwardConfig configOf(const pb::WalkForwardRequest &Request) {
  const core::WalkForwardConfig Defaults;
  core::WalkForwardConfig Config;
  Config.FitDays = orDefault(Request.fit_days(), Defaults.FitDays, "fit_days");
  Config.TestDays =
      orDefault(Request.test_days(), Defaults.TestDays, "test_days");
  Config.ShiftDays =
      orDefault(Request.shift_days(), Defaults.ShiftDays, "shift_days");
  Config.MaxGrid = orDefault(Request.max_grid(), Defaults.MaxGrid, "max_grid");
  return Config;
}

// Московская дата каждого торгового дня шкалы (индекс - номер дня с 1).
std::vector<std::int32_t> datesOfDays(const core::Timeline &Timeline) {
  std::vector<std::int32_t> Dates(
      static_cast<std::size_t>(Timeline.tradingDays()) + 1, 0);
  for (std::int64_t Step = 0; Step < Timeline.totalSteps(); ++Step) {
    Dates[static_cast<std::size_t>(Timeline.tradingDayOf(Step))] =
        Timeline.yyyymmddOf(Step);
  }
  return Dates;
}

void fillResult(const core::WalkForwardResult &Result, const Strategy &Strategy,
                const core::Timeline &Timeline, pb::WalkForwardResult *Out) {
  const std::vector<std::int32_t> Dates = datesOfDays(Timeline);
  const auto DateOf = [&](std::int32_t Day) {
    return formatIsoDate(Dates[static_cast<std::size_t>(Day)]);
  };
  for (const core::WalkForwardSegment &S : Result.Segments) {
    pb::WalkForwardSegment *Segment = Out->add_segments();
    Segment->set_index(S.Index);
    Segment->set_fit_from(DateOf(S.FitFromDay));
    Segment->set_fit_to(DateOf(S.FitToDay));
    Segment->set_test_from(DateOf(S.TestFromDay));
    Segment->set_test_to(DateOf(S.TestToDay));
    for (const core::dsl::ResolvedParam &P :
         Strategy.Params) { // порядок объявления, не алфавитный
      pb::ParamValue *Value = Segment->add_chosen();
      Value->set_name(P.Name);
      Value->set_value(S.Chosen.at(P.Name));
    }
    Segment->set_fit_net_return(S.FitNetReturn);
    // Шаги и сделки проверки - в нумерации среза шкалы: перевод идёт по нему.
    const core::Timeline TestTimeline =
        Timeline.slice(S.TestFromDay, S.TestToDay);
    toProto(S.Test, S.TestReport, std::nullopt, TestTimeline,
            Segment->mutable_test_result());
  }
  Out->set_chained_return(Result.ChainedReturn);
}

} // namespace

// Ни одно исключение не выходит наружу (statusFromCurrentException). Результат
// собирается целиком и пишется в поток последним сообщением; до него идёт
// только прогресс.
grpc::Status handleRunWalkForward(
    Context &Context, const pb::WalkForwardRequest &Request,
    grpc::ServerWriterInterface<pb::WalkForwardUpdate> *Writer) {
  try {
    const core::SessionSpec Spec = toSessionSpec(Request.market());
    const core::WalkForwardConfig Config = configOf(Request);
    const Strategy Strategy = loadStrategy(Request.source());
    const std::shared_ptr<const core::Timeline> Timeline =
        loadTimeline(Context, Request.market());

    const core::WalkForwardResult Result =
        core::walkForward(Spec, Timeline, Strategy.Program, Strategy.Params,
                          Config, progressWriter(Writer));

    pb::WalkForwardUpdate Update;
    fillResult(Result, Strategy, *Timeline, Update.mutable_result());
    if (!Writer->Write(Update))
      return grpc::Status(grpc::StatusCode::CANCELLED, "клиент закрыл поток");
    return grpc::Status::OK;
  } catch (const StreamClosed &) {
    return grpc::Status(grpc::StatusCode::CANCELLED, "клиент закрыл поток");
  } catch (...) {
    return statusFromCurrentException();
  }
}

} // namespace worker
