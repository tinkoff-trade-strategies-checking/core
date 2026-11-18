#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/report/benchmark.hpp"

// Классификатор категории исторического периода (`decisions.md`,
// «Стресс-сценарии» -> «Критерии категорий», INV-4):
// - по дневным close IMOEX на отрезке длиной L >= 3 торговых дней;
// - sigma = выборочное stdev(ln(C_d / C_{d-1})) * sqrt(252);
// - mdd = max(1 - C_t / max_{s <= t} C_s);
// - пороги P33 и P80 по sigma среди всех окон длины L в снапшоте;
// - категория (при совпадении условий, например P33 == P80, побеждает Stress):
//     Stress — sigma >= P80 или mdd >= 0.20;
//     Calm   — sigma <= P33 и mdd < 0.10;
//     Medium — иначе.

namespace core {

enum class PeriodCategory { Calm, Medium, Stress };

struct PeriodWindow {
  std::int32_t StartDate = 0; // yyyymmdd
  std::int32_t EndDate = 0;   // yyyymmdd
  double Sigma = 0.0;
  double Mdd = 0.0;
  PeriodCategory Category = PeriodCategory::Medium;
};

struct PeriodClassification {
  std::vector<PeriodWindow> Windows;
  double P33 = 0.0;
  double P80 = 0.0;
};

// Все окна длины WindowLength торговых дней по возрастанию StartDate (INV-4).
// WindowLength < 3, неупорядоченные даты или close <= 0 ->
// std::invalid_argument.
PeriodClassification classifyPeriods(std::span<const DailyClose> Series,
                                     std::int32_t WindowLength);

// Вариант для одного окна («своя дата»): пороги P33/P80 считаются по всем
// окнам длины WindowLength в Series, возвращается окно с началом StartDate.
// Если окна с таким StartDate нет -> std::invalid_argument.
PeriodClassification classifyPeriodAt(std::span<const DailyClose> Series,
                                      std::int32_t WindowLength,
                                      std::int32_t StartDate);

} // namespace core
