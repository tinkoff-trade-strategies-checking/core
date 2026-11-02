// Эталонный портфель (INV-6): 19 сделок по двум бумагам, таблица посчитана независимой реализацией
// правил `decisions.md` (Python `decimal`) и лежит в testdata/reference-portfolio.md.
// Тест вызывает fillPrice, amounts и Portfolio::applyFill и сверяет каждую строку до копейки.
#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/exec/costs.hpp"
#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp"

namespace {

using core::CostConfig;
using core::Decimal;
using core::Portfolio;
using core::RoundingMode;
using core::Side;

constexpr std::int64_t kLotA = 10;
constexpr std::int64_t kLotB = 10000;
const char* const kNoPosition = "\xE2\x80\x94";  // «—» (U+2014): позиции нет

Decimal D(const std::string& text) { return Decimal::parse(text); }

struct Row {
  int number = 0;
  int instrument = 0;  // 0 - A, 1 - B
  Side side = Side::Buy;
  std::int64_t lots = 0, barVolume = 0, positionAfter = 0;
  Decimal base, slippage, price, value, commission, fee, cashAfter, avgAfter;
};

std::string trim(const std::string& s) {
  const auto first = s.find_first_not_of(" \t\r");
  if (first == std::string::npos) return "";
  return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}

std::vector<std::string> splitCells(const std::string& line) {
  std::vector<std::string> cells;
  std::stringstream ss(line);
  std::string cell;
  while (std::getline(ss, cell, '|')) cells.push_back(trim(cell));
  // Строка начинается и заканчивается «|»: первая пустая ячейка слева, возможная пустая справа.
  if (!cells.empty() && cells.front().empty()) cells.erase(cells.begin());
  if (!cells.empty() && cells.back().empty()) cells.pop_back();
  return cells;
}

// Строки сделок: таблица, где первая ячейка - номер сделки. Заголовок и разделитель пропускаются.
std::vector<Row> loadReferenceRows() {
  std::ifstream in(std::string(CORE_TESTDATA_DIR) + "/reference-portfolio.md", std::ios::binary);
  EXPECT_TRUE(in.is_open()) << "нет testdata/reference-portfolio.md";
  std::vector<Row> rows;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line.front() != '|') continue;
    const auto cells = splitCells(line);
    if (cells.size() != 15 || cells[0].empty() || cells[0].find_first_not_of("0123456789") != std::string::npos) {
      continue;
    }
    Row r;
    r.number = std::stoi(cells[0]);
    EXPECT_TRUE(cells[1] == "A" || cells[1] == "B") << "сделка " << r.number;
    r.instrument = cells[1] == "A" ? 0 : 1;
    EXPECT_TRUE(cells[2] == "BUY" || cells[2] == "SELL") << "сделка " << r.number;
    r.side = cells[2] == "BUY" ? Side::Buy : Side::Sell;
    r.lots = std::stoll(cells[3]);
    r.base = D(cells[4]);
    r.barVolume = std::stoll(cells[5]);
    r.slippage = D(cells[6]);
    // cells[7] - O·m до шага цены, промежуточное значение: проверяется косвенно через итоговую цену.
    r.price = D(cells[8]);
    r.value = D(cells[9]);
    r.commission = D(cells[10]);
    r.fee = D(cells[11]);
    r.cashAfter = D(cells[12]);
    r.positionAfter = std::stoll(cells[13]);
    r.avgAfter = cells[14] == kNoPosition ? Decimal::zero() : D(cells[14]);
    rows.push_back(r);
  }
  return rows;
}

TEST(ReferencePortfolio, TableHas19Rows) {
  const auto rows = loadReferenceRows();
  ASSERT_EQ(rows.size(), 19u);
  for (std::size_t i = 0; i < rows.size(); ++i) EXPECT_EQ(rows[i].number, static_cast<int>(i) + 1);
}

TEST(ReferencePortfolio, EveryTradeMatchesTable) {
  const auto rows = loadReferenceRows();
  ASSERT_EQ(rows.size(), 19u);

  const CostConfig costs = CostConfig::defaults();
  const Decimal steps[2] = {D("0.01"), D("0.000005")};
  const std::int64_t lotSizes[2] = {kLotA, kLotB};
  Portfolio portfolio(D("1000000.00"), 2);
  Decimal totalCommission, totalFee;

  for (const Row& r : rows) {
    SCOPED_TRACE("сделка " + std::to_string(r.number));

    // Доля проскальзывания κ·q/V, 9 знаков HALF_UP.
    EXPECT_EQ(costs.slippageKappa.times(r.lots).div(Decimal::fromUnits(r.barVolume), 9, RoundingMode::HALF_UP),
              r.slippage);

    const Decimal price = core::fillPrice(r.side, r.base, r.lots, r.barVolume, costs, steps[r.instrument]);
    EXPECT_EQ(price, r.price);

    const core::Amounts a = core::amounts(price, r.lots, lotSizes[r.instrument], costs);
    EXPECT_EQ(a.value, r.value);
    EXPECT_EQ(a.commission, r.commission);
    EXPECT_EQ(a.fee, r.fee);

    portfolio.applyFill(r.instrument, r.side, r.lots, price, a.value, a.commission, a.fee, r.number, r.number);
    totalCommission += a.commission;
    totalFee += a.fee;

    EXPECT_EQ(portfolio.cash(), r.cashAfter);
    EXPECT_EQ(portfolio.lots(r.instrument), r.positionAfter);
    EXPECT_EQ(portfolio.avgPrice(r.instrument), r.avgAfter);
  }

  // Итоги таблицы.
  EXPECT_EQ(portfolio.cash(), D("1076446.62"));
  EXPECT_EQ(portfolio.lots(0), 20);
  EXPECT_EQ(portfolio.lots(1), 500);
  EXPECT_EQ(totalCommission, D("1980.42"));
  EXPECT_EQ(totalFee, D("396.16"));

  const std::vector<std::optional<Decimal>> lastClose = {D("260.00"), D("0.0300")};
  const std::vector<std::int64_t> lotSizesVec = {kLotA, kLotB};
  const Decimal equity = portfolio.equity(lastClose, lotSizesVec);
  EXPECT_EQ(equity, D("1278446.62"));
  EXPECT_EQ(equity.toString(2), "1278446.62");
}

}  // namespace
