#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/market/bar.hpp"

namespace core {

// Курсор по загруженной серии баров: единственное место, где для рыночных данных обеспечивается
// INV-1 (стратегия и пользователь видят только данные, доступные «сейчас»). Наружу отдаётся
// только префикс серии [0 .. current]; доступа к элементам после current нет ни через какой метод.
//
// Исходное положение - ДО первого бара: current() не определён, visible() пуст. Цикл симуляции:
//   BarCursor cursor(source.load(request));
//   while (cursor.advance()) { /* cursor.current(), cursor.visible() */ }
class BarCursor {
 public:
  // Серия должна быть корректной: bars[i].index == i, tsOpenMs строго возрастает,
  // availableAtMs >= tsOpenMs; иначе std::invalid_argument.
  explicit BarCursor(std::vector<Bar> bars);

  // Переходит к следующему бару. false - серия закончилась (положение не меняется).
  bool advance();
  // Есть ли следующий бар (содержимое будущего не раскрывается).
  bool hasNext() const { return next_ < bars_.size(); }
  bool started() const { return next_ > 0; }

  // Текущий бар. До первого advance() - std::logic_error.
  const Bar& current() const;
  // Индекс текущего бара; nullopt до первого advance().
  std::optional<std::int64_t> currentIndex() const;
  // Все бары с index <= current; никогда не включает current + 1. До первого advance() пусто.
  std::span<const Bar> visible() const;

 private:
  std::vector<Bar> bars_;
  std::size_t next_ = 0;  // число уже показанных баров; текущий - bars_[next_ - 1]
};

}  // namespace core
