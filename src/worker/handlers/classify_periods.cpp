#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "convert/market.hpp"
#include "core/analysis/period_classifier.hpp"
#include "core/market/bar_source.hpp"
#include "core/report/benchmark.hpp"
#include "handlers.hpp"

namespace worker {

namespace {

pb::PeriodCategory toProto(core::PeriodCategory Category) {
  switch (Category) {
  case core::PeriodCategory::Calm:
    return pb::CALM;
  case core::PeriodCategory::Medium:
    return pb::MEDIUM;
  case core::PeriodCategory::Stress:
    return pb::STRESS;
  }
  return pb::PERIOD_CATEGORY_UNSPECIFIED;
}

void fill(Context &Context, const pb::ClassifyPeriodsRequest &Request,
          pb::ClassifyPeriodsResponse *Out) {
  if (Request.snapshot_id().empty())
    throw std::invalid_argument("не указан snapshot_id");
  if (Request.window_length() < 3) {
    throw std::invalid_argument(
        "длина окна должна быть не меньше 3 торговых дней");
  }

  const std::optional<std::int32_t> FromYmd =
      Request.from_date().empty()
          ? std::nullopt
          : std::optional<std::int32_t>(parseIsoDate(Request.from_date()));
  const std::optional<std::int32_t> ToYmd =
      Request.to_date().empty()
          ? std::nullopt
          : std::optional<std::int32_t>(parseIsoDate(Request.to_date()));
  if (FromYmd && ToYmd && *FromYmd > *ToYmd) {
    throw std::invalid_argument("from_date больше to_date");
  }

  // Пороги P33/P80 считаются по ВСЕМ окнам длины L во всём снапшоте
  // (границы хранения ClickHouse Date: 1970-01-01..2100-01-01).
  pb::MarketSpec BenchmarkSpec;
  BenchmarkSpec.set_snapshot_id(Request.snapshot_id());
  BenchmarkSpec.set_from_date("1970-01-01");
  BenchmarkSpec.set_to_date("2100-01-01");
  BenchmarkSpec.set_benchmark_ticker(Request.benchmark_ticker().empty()
                                         ? "IMOEX"
                                         : Request.benchmark_ticker());

  const std::optional<core::BenchmarkSeries> Series =
      loadBenchmark(Context, BenchmarkSpec);
  if (!Series || Series->Closes.empty())
    throw core::SnapshotNotFound("в снапшоте нет дневного ряда индекса");

  const core::PeriodClassification Result =
      Request.has_start_date()
          ? core::classifyPeriodAt(Series->Closes, Request.window_length(),
                                   parseIsoDate(Request.start_date()))
          : core::classifyPeriods(Series->Closes, Request.window_length());

  Out->set_p33(Result.P33);
  Out->set_p80(Result.P80);
  for (const core::PeriodWindow &W : Result.Windows) {
    if (!Request.has_start_date()) {
      if (FromYmd && W.StartDate < *FromYmd)
        continue;
      if (ToYmd && W.EndDate > *ToYmd)
        continue;
    }
    pb::PeriodWindow *Win = Out->add_windows();
    Win->set_start_date(formatIsoDate(W.StartDate));
    Win->set_end_date(formatIsoDate(W.EndDate));
    Win->set_sigma(W.Sigma);
    Win->set_mdd(W.Mdd);
    Win->set_category(toProto(W.Category));
  }
}

} // namespace

grpc::Status handleClassifyPeriods(Context &Context,
                                   const pb::ClassifyPeriodsRequest &Request,
                                   pb::ClassifyPeriodsResponse *Response) {
  try {
    pb::ClassifyPeriodsResponse Result;
    fill(Context, Request, &Result);
    *Response = std::move(Result);
    return grpc::Status::OK;
  } catch (...) {
    return statusFromCurrentException();
  }
}

} // namespace worker
