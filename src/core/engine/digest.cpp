#include "core/engine/digest.hpp"

#include <cstdint>
#include <string>

namespace core {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

const char *statusName(SessionStatus Status) {
  switch (Status) {
  case SessionStatus::Running:
    return "Running";
  case SessionStatus::FinishedEnd:
    return "FinishedEnd";
  case SessionStatus::Bankrupt:
    return "Bankrupt";
  }
  return "?";
}

} // namespace

// Канонический текст: через '\n' сделки
// (orderId|step|instrument|side|lots|price|value|commission|fee), точки кривой
// (step|equity) и статус. Деньги - Decimal::toString(), без double (INV-4,
// INV-6).
std::uint64_t digestOf(const SessionEngine &Engine) {
  std::string Text;
  const auto Line = [&Text](const std::string &S) {
    if (!Text.empty())
      Text += '\n';
    Text += S;
  };
  for (const Fill &F : Engine.fills()) {
    Line(std::to_string(F.OrderId) + '|' + std::to_string(F.Step) + '|' +
         std::to_string(F.Instrument) + '|' +
         (F.OrderSide == Side::Buy ? "buy" : "sell") + '|' +
         std::to_string(F.Lots) + '|' + F.Price.toString() + '|' +
         F.Value.toString() + '|' + F.Commission.toString() + '|' +
         F.Fee.toString());
  }
  for (const EquityPoint &P : Engine.equity())
    Line(std::to_string(P.Step) + '|' + P.Equity.toString());
  Line(statusName(Engine.status()));

  std::uint64_t Hash = kFnvOffset;
  for (const char C : Text) {
    Hash ^= static_cast<std::uint8_t>(C);
    Hash *= kFnvPrime;
  }
  return Hash;
}

} // namespace core
