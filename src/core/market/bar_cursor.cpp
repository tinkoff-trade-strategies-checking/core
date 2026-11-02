#include "core/market/bar_cursor.hpp"

#include <stdexcept>
#include <utility>

namespace core {

BarCursor::BarCursor(std::vector<Bar> bars) : bars_(std::move(bars)) {
  for (std::size_t i = 0; i < bars_.size(); ++i) {
    if (bars_[i].index != static_cast<std::int64_t>(i)) {
      throw std::invalid_argument("BarCursor: индексы баров должны идти 0, 1, 2, ... без пропусков");
    }
    if (bars_[i].availableAtMs < bars_[i].tsOpenMs) {
      throw std::invalid_argument("BarCursor: availableAtMs не может быть раньше tsOpenMs");
    }
    if (i > 0 && bars_[i].tsOpenMs <= bars_[i - 1].tsOpenMs) {
      throw std::invalid_argument("BarCursor: tsOpenMs должен строго возрастать");
    }
  }
}

bool BarCursor::advance() {
  if (next_ >= bars_.size()) return false;
  ++next_;
  return true;
}

const Bar& BarCursor::current() const {
  if (next_ == 0) throw std::logic_error("BarCursor: current() до первого advance()");
  return bars_[next_ - 1];
}

std::optional<std::int64_t> BarCursor::currentIndex() const {
  if (next_ == 0) return std::nullopt;
  return bars_[next_ - 1].index;
}

std::span<const Bar> BarCursor::visible() const {
  return std::span<const Bar>(bars_.data(), next_);
}

}  // namespace core
