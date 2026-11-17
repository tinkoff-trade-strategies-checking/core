#pragma once

#include <cstdint>
#include <string>

#include "core/money/proto_conv.hpp"
#include "handlers.hpp"
#include "support/fixture1.hpp"

// Общие помощники тестов воркера: MarketSpec на снапшот fixture-1 и сборка
// заявок журнала.

namespace worker_test {

namespace pb = pac::simulation::v1;

inline pb::Decimal dec(const char *Text) {
  return core::toProto<pb::Decimal>(core::Decimal::parse(Text));
}

// fixture-1: A (FIXA, лот 10, шаг 0.01) и B (FIXB, лот 10000, шаг 0.000005),
// D1, 10 шагов, у B нет бара на шаге 3.
inline pb::MarketSpec fixtureMarket() {
  pb::MarketSpec Market;
  Market.set_snapshot_id("fixture-1");
  pb::Instrument *A = Market.add_instruments();
  A->set_key("A");
  A->set_ticker("FIXA");
  A->set_lot_size(10);
  *A->mutable_price_step() = dec("0.01");
  pb::Instrument *B = Market.add_instruments();
  B->set_key("B");
  B->set_ticker("FIXB");
  B->set_lot_size(10000);
  *B->mutable_price_step() = dec("0.000005");
  Market.set_timeframe(pb::D1);
  Market.set_from_date("2024-01-09");
  Market.set_to_date("2024-01-22");
  pb::CostConfig *Costs = Market.mutable_costs();
  *Costs->mutable_commission_rate() = dec("0.0005");
  *Costs->mutable_exchange_fee_rate() = dec("0.0001");
  *Costs->mutable_spread() = dec("0.001");
  *Costs->mutable_slippage_kappa() = dec("0.1");
  *Costs->mutable_liquidity_share() = dec("0.10");
  *Market.mutable_initial_cash() = dec("1000000");
  return Market;
}

inline pb::JournalEntry marketOrder(std::int64_t Seq, std::int64_t Step,
                                    const char *Key, pb::Side Side,
                                    std::int64_t Lots) {
  pb::JournalEntry E;
  E.set_seq(Seq);
  E.set_step(Step);
  pb::OrderSpec *Spec = E.mutable_place()->mutable_order();
  Spec->set_instrument_key(Key);
  Spec->set_side(Side);
  Spec->set_type(pb::MARKET);
  Spec->set_lots(Lots);
  Spec->set_time_in_force(pb::GTC);
  return E;
}

} // namespace worker_test
