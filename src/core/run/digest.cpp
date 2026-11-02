#include <cstdint>
#include <string>

#include "core/run/backtest.hpp"

namespace core {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

const char* statusName(RunStatus status) {
  switch (status) {
    case RunStatus::Completed: return "Completed";
    case RunStatus::BankruptStop: return "BankruptStop";
    case RunStatus::Invalid: return "Invalid";
  }
  return "?";
}

}  // namespace

// Канонический текст: через '\n' сделки (orderId|step|instrument|side|lots|price|value|commission|fee),
// точки кривой (step|equity) и статус. Деньги - Decimal::toString(), без double (INV-4, INV-6).
std::uint64_t digestOf(const RunOutput& output) {
  std::string text;
  const auto line = [&text](const std::string& s) {
    if (!text.empty()) text += '\n';
    text += s;
  };
  for (const Fill& f : output.fills) {
    line(std::to_string(f.orderId) + '|' + std::to_string(f.step) + '|' + std::to_string(f.instrument) + '|' +
         (f.side == Side::Buy ? "buy" : "sell") + '|' + std::to_string(f.lots) + '|' + f.price.toString() + '|' +
         f.value.toString() + '|' + f.commission.toString() + '|' + f.fee.toString());
  }
  for (const EquityPoint& p : output.equity) line(std::to_string(p.step) + '|' + p.equity.toString());
  line(statusName(output.status));

  std::uint64_t hash = kFnvOffset;
  for (const char c : text) {
    hash ^= static_cast<std::uint8_t>(c);
    hash *= kFnvPrime;
  }
  return hash;
}

}  // namespace core
