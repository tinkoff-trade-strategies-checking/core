// Эталонный портфель (INV-6): 19 сделок по двум бумагам, таблица посчитана
// независимой реализацией правил `decisions.md` (Python `decimal`) и лежит в
// testdata/reference-portfolio.md. Тест вызывает fillPrice, amounts и
// Portfolio::applyFill и сверяет каждую строку до копейки.
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
const char *const kNoPosition = "\xE2\x80\x94"; // «—» (U+2014): позиции нет

Decimal d(const std::string &Text) { return Decimal::parse(Text); }

struct Row {
  int Number = 0;
  int Instrument = 0; // 0 - A, 1 - B
  Side OrderSide = Side::Buy;
  std::int64_t Lots = 0, BarVolume = 0, PositionAfter = 0;
  Decimal Base, Slippage, Price, Value, Commission, Fee, CashAfter, AvgAfter;
};

std::string trim(const std::string &S) {
  const auto First = S.find_first_not_of(" \t\r");
  if (First == std::string::npos)
    return "";
  return S.substr(First, S.find_last_not_of(" \t\r") - First + 1);
}

std::vector<std::string> splitCells(const std::string &Line) {
  std::vector<std::string> Cells;
  std::stringstream Ss(Line);
  std::string Cell;
  while (std::getline(Ss, Cell, '|'))
    Cells.push_back(trim(Cell));
  // Строка начинается и заканчивается «|»: первая пустая ячейка слева,
  // возможная пустая справа.
  if (!Cells.empty() && Cells.front().empty())
    Cells.erase(Cells.begin());
  if (!Cells.empty() && Cells.back().empty())
    Cells.pop_back();
  return Cells;
}

// Строки сделок: таблица, где первая ячейка - номер сделки. Заголовок и
// разделитель пропускаются.
std::vector<Row> loadReferenceRows() {
  std::ifstream In(std::string(CORE_TESTDATA_DIR) + "/reference-portfolio.md",
                   std::ios::binary);
  EXPECT_TRUE(In.is_open()) << "нет testdata/reference-portfolio.md";
  std::vector<Row> Rows;
  std::string Line;
  while (std::getline(In, Line)) {
    if (Line.empty() || Line.front() != '|')
      continue;
    const auto Cells = splitCells(Line);
    if (Cells.size() != 15 || Cells[0].empty() ||
        Cells[0].find_first_not_of("0123456789") != std::string::npos) {
      continue;
    }
    Row R;
    R.Number = std::stoi(Cells[0]);
    EXPECT_TRUE(Cells[1] == "A" || Cells[1] == "B") << "сделка " << R.Number;
    R.Instrument = Cells[1] == "A" ? 0 : 1;
    EXPECT_TRUE(Cells[2] == "BUY" || Cells[2] == "SELL")
        << "сделка " << R.Number;
    R.OrderSide = Cells[2] == "BUY" ? Side::Buy : Side::Sell;
    R.Lots = std::stoll(Cells[3]);
    R.Base = d(Cells[4]);
    R.BarVolume = std::stoll(Cells[5]);
    R.Slippage = d(Cells[6]);
    // cells[7] - O·m до шага цены, промежуточное значение: проверяется косвенно
    // через итоговую цену.
    R.Price = d(Cells[8]);
    R.Value = d(Cells[9]);
    R.Commission = d(Cells[10]);
    R.Fee = d(Cells[11]);
    R.CashAfter = d(Cells[12]);
    R.PositionAfter = std::stoll(Cells[13]);
    R.AvgAfter = Cells[14] == kNoPosition ? Decimal::zero() : d(Cells[14]);
    Rows.push_back(R);
  }
  return Rows;
}

TEST(ReferencePortfolio, TableHas19Rows) {
  const auto Rows = loadReferenceRows();
  ASSERT_EQ(Rows.size(), 19u);
  for (std::size_t I = 0; I < Rows.size(); ++I)
    EXPECT_EQ(Rows[I].Number, static_cast<int>(I) + 1);
}

TEST(ReferencePortfolio, EveryTradeMatchesTable) {
  const auto Rows = loadReferenceRows();
  ASSERT_EQ(Rows.size(), 19u);

  const CostConfig Costs = CostConfig::defaults();
  const Decimal Steps[2] = {d("0.01"), d("0.000005")};
  const std::int64_t LotSizes[2] = {kLotA, kLotB};
  Portfolio Portfolio(d("1000000.00"), 2);
  Decimal TotalCommission, TotalFee;

  for (const Row &R : Rows) {
    SCOPED_TRACE("сделка " + std::to_string(R.Number));

    // Доля проскальзывания κ·q/V, 9 знаков HALF_UP.
    EXPECT_EQ(Costs.SlippageKappa.times(R.Lots).div(
                  Decimal::fromUnits(R.BarVolume), 9, RoundingMode::HalfUp),
              R.Slippage);

    const Decimal Price = core::fillPrice(
        R.OrderSide, R.Base, R.Lots, R.BarVolume, Costs, Steps[R.Instrument]);
    EXPECT_EQ(Price, R.Price);

    const core::Amounts A =
        core::amounts(Price, R.Lots, LotSizes[R.Instrument], Costs);
    EXPECT_EQ(A.Value, R.Value);
    EXPECT_EQ(A.Commission, R.Commission);
    EXPECT_EQ(A.Fee, R.Fee);

    Portfolio.applyFill(R.Instrument, R.OrderSide, R.Lots, Price, A.Value,
                        A.Commission, A.Fee, R.Number, R.Number);
    TotalCommission += A.Commission;
    TotalFee += A.Fee;

    EXPECT_EQ(Portfolio.cash(), R.CashAfter);
    EXPECT_EQ(Portfolio.lots(R.Instrument), R.PositionAfter);
    EXPECT_EQ(Portfolio.avgPrice(R.Instrument), R.AvgAfter);
  }

  // Итоги таблицы.
  EXPECT_EQ(Portfolio.cash(), d("1066544.85"));
  EXPECT_EQ(Portfolio.lots(0), 20);
  EXPECT_EQ(Portfolio.lots(1), 500);
  EXPECT_EQ(TotalCommission, d("11882.19"));
  EXPECT_EQ(TotalFee, d("396.16"));

  const std::vector<std::optional<Decimal>> LastClose = {d("260.00"),
                                                         d("0.0300")};
  const std::vector<std::int64_t> LotSizesVec = {kLotA, kLotB};
  const Decimal Equity = Portfolio.equity(LastClose, LotSizesVec);
  EXPECT_EQ(Equity, d("1268544.85"));
  EXPECT_EQ(Equity.toString(2), "1268544.85");
}

} // namespace
