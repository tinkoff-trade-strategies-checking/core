#pragma once

#include <cstdint>
#include <string>

#include "core/money/proto_conv.hpp"
#include "handlers.hpp"
#include "support/fixture1.hpp"

// Общие помощники тестов воркера: MarketSpec на снапшот fixture-1 и сборка заявок журнала.

namespace worker_test {

namespace pb = pac::simulation::v1;

inline pb::Decimal dec(const char* text) { return core::toProto<pb::Decimal>(core::Decimal::parse(text)); }

// fixture-1: A (FIXA, лот 10, шаг 0.01) и B (FIXB, лот 10000, шаг 0.000005), D1, 10 шагов, у B нет бара на шаге 3.
inline pb::MarketSpec fixtureMarket() {
  pb::MarketSpec market;
  market.set_snapshot_id("fixture-1");
  pb::Instrument* a = market.add_instruments();
  a->set_key("A");
  a->set_ticker("FIXA");
  a->set_lot_size(10);
  *a->mutable_price_step() = dec("0.01");
  pb::Instrument* b = market.add_instruments();
  b->set_key("B");
  b->set_ticker("FIXB");
  b->set_lot_size(10000);
  *b->mutable_price_step() = dec("0.000005");
  market.set_timeframe(pb::D1);
  market.set_from_date("2024-01-09");
  market.set_to_date("2024-01-22");
  pb::CostConfig* costs = market.mutable_costs();
  *costs->mutable_commission_rate() = dec("0.0005");
  *costs->mutable_exchange_fee_rate() = dec("0.0001");
  *costs->mutable_spread() = dec("0.001");
  *costs->mutable_slippage_kappa() = dec("0.1");
  *costs->mutable_liquidity_share() = dec("0.10");
  *market.mutable_initial_cash() = dec("1000000");
  return market;
}

inline pb::JournalEntry marketOrder(std::int64_t seq, std::int64_t step, const char* key, pb::Side side,
                                    std::int64_t lots) {
  pb::JournalEntry e;
  e.set_seq(seq);
  e.set_step(step);
  pb::OrderSpec* spec = e.mutable_place()->mutable_order();
  spec->set_instrument_key(key);
  spec->set_side(side);
  spec->set_type(pb::MARKET);
  spec->set_lots(lots);
  spec->set_time_in_force(pb::GTC);
  return e;
}

}  // namespace worker_test
