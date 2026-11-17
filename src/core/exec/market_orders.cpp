#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>

#include "core/exec/execution.hpp"
#include "exec_common.hpp"

namespace core {

namespace {

// Общий обход рыночных заявок. naive = false: заявки, поданные раньше шага, по
// open бара шага своей бумаги, с издержками и бюджетом ликвидности. naive =
// true (ФТ-24): заявки, поданные не позже шага, по close последнего бара бумаги
// без издержек и лимита. budget - бюджет ликвидности шага (nullptr в режиме
// naive).
void runMarketOrders(bool naive, std::int64_t step,
                     const std::vector<Instrument> &instruments,
                     const CostConfig &costs, const Timeline &timeline,
                     LiquidityBudget *budget, std::vector<Order> &orders,
                     Portfolio &portfolio, std::vector<Fill> &fills,
                     std::vector<std::int64_t> &changed) {
  const CostConfig zeroCosts = CostConfig::zero();
  const CostConfig &cfg = naive ? zeroCosts : costs;

  // Заявки - по возрастанию id.
  std::vector<std::size_t> order(orders.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) {
                     return orders[a].id < orders[b].id;
                   });

  for (const std::size_t index : order) {
    Order &o = orders[index];
    if (o.spec.type != OrderType::Market)
      continue;
    if (o.status != OrderStatus::Active &&
        o.status != OrderStatus::PartiallyFilled)
      continue;
    if (naive ? o.placedStep > step : o.placedStep >= step)
      continue; // INV-2: не раньше следующего бара

    const int instrument = o.spec.instrument;
    const Instrument &inst = instruments[static_cast<std::size_t>(instrument)];
    // Заявка ждёт бара своей бумаги: чужие бары шага ей не подходят.
    const StepBar *bar = naive ? timeline.lastBarOf(instrument, step)
                               : timeline.barOf(instrument, step);
    if (bar == nullptr)
      continue;

    const Decimal base = naive ? bar->close : bar->open;
    // nullopt - без лимита (ФТ-24 и liquidityShare не задан); бар без объёма
    // при заданном лимите даёт 0.
    const std::optional<std::int64_t> cap =
        budget != nullptr ? budget->remaining(instrument, *bar, costs)
                          : std::nullopt;

    auto quote = [&](std::int64_t q) {
      const Decimal price =
          naive ? base
                : fillPrice(o.spec.side, base, q, bar->volumeLots, cfg,
                            inst.priceStep);
      return Quote{price, amounts(price, q, inst.lotSize, cfg)};
    };
    const std::int64_t remaining = o.spec.lots - o.filledLots;
    const auto [q, reason] =
        chooseQuantity(o.spec.side, remaining, cap, portfolio.lots(instrument),
                       portfolio.cash(), quote);

    if (q > 0) {
      const Quote x = quote(q);
      recordFill(portfolio, fills, o, step, q, x, base, inst, cfg);
      o.filledLots += q;
      if (budget != nullptr)
        budget->consume(instrument, q);
    }
    // Остаток рыночной заявки не переносится: заявка исполнена целиком или
    // снята с причиной.
    if (reason == Reason::None) {
      o.status = OrderStatus::Filled;
    } else {
      o.status = OrderStatus::Cancelled;
      o.reason = reason;
    }
    o.closedStep = step;
    changed.push_back(o.id);
  }
}

} // namespace

void ExecutionEngine::processMarket(std::int64_t step, const Timeline &timeline,
                                    LiquidityBudget &budget,
                                    std::vector<Order> &orders,
                                    Portfolio &portfolio,
                                    std::vector<Fill> &fills,
                                    std::vector<std::int64_t> &changed) const {
  runMarketOrders(false, step, instruments_, costs_, timeline, &budget, orders,
                  portfolio, fills, changed);
}

void ExecutionEngine::processNaive(std::int64_t step, const Timeline &timeline,
                                   std::vector<Order> &orders,
                                   Portfolio &portfolio,
                                   std::vector<Fill> &fills,
                                   std::vector<std::int64_t> &changed) const {
  if (!naive_)
    throw std::logic_error(
        "processNaive: движок создан без режима naive (ФТ-24)");
  runMarketOrders(true, step, instruments_, costs_, timeline, nullptr, orders,
                  portfolio, fills, changed);
}

} // namespace core
