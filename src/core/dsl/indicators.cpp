#include "core/dsl/indicators.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace core::dsl {
namespace {

// Окно последних n значений; порядок обхода — от старого к новому.
class Window {
 public:
  explicit Window(std::size_t capacity) : buf_(capacity), capacity_(capacity) {}

  void push(double v) {
    buf_[head_] = v;
    head_ = (head_ + 1) % capacity_;
    if (size_ < capacity_) ++size_;
  }
  bool full() const { return size_ == capacity_; }
  // i = 0 — самое старое значение окна.
  double at(std::size_t i) const {
    const std::size_t start = (head_ + capacity_ - size_) % capacity_;
    return buf_[(start + i) % capacity_];
  }
  std::size_t size() const { return size_; }

 private:
  std::vector<double> buf_;
  std::size_t capacity_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
};

void requireInputs(const std::vector<Value>& inputs, std::size_t expected, const char* name) {
  if (inputs.size() != expected) {
    throw std::invalid_argument(std::string("индикатор ") + name + ": ожидается входов " +
                                std::to_string(expected) + ", получено " +
                                std::to_string(inputs.size()));
  }
}

bool anyNa(const std::vector<Value>& inputs) {
  return std::any_of(inputs.begin(), inputs.end(), [](const Value& v) { return !v.has_value(); });
}

double windowSum(const Window& w) {
  double sum = 0.0;
  for (std::size_t i = 0; i < w.size(); ++i) sum += w.at(i);
  return sum;
}

class Sma final : public Indicator {
 public:
  explicit Sma(std::size_t n) : n_(static_cast<double>(n)), window_(n) {}
  Value update(const std::vector<Value>& inputs) override {
    requireInputs(inputs, 1, "sma");
    if (anyNa(inputs)) return std::nullopt;
    window_.push(*inputs[0]);
    if (!window_.full()) return std::nullopt;
    return windowSum(window_) / n_;
  }

 private:
  double n_;
  Window window_;
};

class Ema final : public Indicator {
 public:
  explicit Ema(std::size_t n)
      : n_(static_cast<double>(n)), alpha_(2.0 / (static_cast<double>(n) + 1.0)), seed_(n) {}
  Value update(const std::vector<Value>& inputs) override {
    requireInputs(inputs, 1, "ema");
    if (anyNa(inputs)) return std::nullopt;
    const double x = *inputs[0];
    if (!seeded_) {
      seed_.push(x);
      if (!seed_.full()) return std::nullopt;
      prev_ = windowSum(seed_) / n_;
      seeded_ = true;
      return prev_;
    }
    prev_ = alpha_ * x + (1.0 - alpha_) * prev_;
    return prev_;
  }

 private:
  double n_;
  double alpha_;
  Window seed_;
  bool seeded_ = false;
  double prev_ = 0.0;
};

// Сглаживание по Уайлдеру: первое значение — среднее первых n, дальше (prev * (n - 1) + x) / n.
class Wilder {
 public:
  explicit Wilder(std::size_t n) : n_(static_cast<double>(n)), need_(n) {}
  // Возвращает сглаженное значение или nullopt, пока не набралось n значений.
  Value push(double x) {
    if (count_ < need_) {
      sum_ += x;
      ++count_;
      if (count_ < need_) return std::nullopt;
      avg_ = sum_ / n_;
      return avg_;
    }
    avg_ = (avg_ * (n_ - 1.0) + x) / n_;
    return avg_;
  }

 private:
  double n_;
  std::size_t need_;
  std::size_t count_ = 0;
  double sum_ = 0.0;
  double avg_ = 0.0;
};

class Rsi final : public Indicator {
 public:
  explicit Rsi(std::size_t n) : gain_(n), loss_(n) {}
  Value update(const std::vector<Value>& inputs) override {
    requireInputs(inputs, 1, "rsi");
    if (anyNa(inputs)) return std::nullopt;
    const double x = *inputs[0];
    if (!hasPrev_) {
      prev_ = x;
      hasPrev_ = true;
      return std::nullopt;
    }
    const double d = x - prev_;
    prev_ = x;
    const Value g = gain_.push(d > 0.0 ? d : 0.0);
    const Value l = loss_.push(d < 0.0 ? -d : 0.0);
    if (!g || !l) return std::nullopt;
    if (*l == 0.0) return *g == 0.0 ? 50.0 : 100.0;
    return 100.0 - 100.0 / (1.0 + *g / *l);
  }

 private:
  Wilder gain_;
  Wilder loss_;
  bool hasPrev_ = false;
  double prev_ = 0.0;
};

class Atr final : public Indicator {
 public:
  explicit Atr(std::size_t n) : smooth_(n) {}
  Value update(const std::vector<Value>& inputs) override {
    requireInputs(inputs, 3, "atr");
    if (anyNa(inputs)) return std::nullopt;
    const double high = *inputs[0];
    const double low = *inputs[1];
    const double close = *inputs[2];
    double tr = high - low;
    if (hasPrevClose_) {
      tr = std::max(tr, std::max(std::fabs(high - prevClose_), std::fabs(low - prevClose_)));
    }
    prevClose_ = close;
    hasPrevClose_ = true;
    return smooth_.push(tr);
  }

 private:
  Wilder smooth_;
  bool hasPrevClose_ = false;
  double prevClose_ = 0.0;
};

class Stdev final : public Indicator {
 public:
  explicit Stdev(std::size_t n) : n_(static_cast<double>(n)), window_(n) {}
  Value update(const std::vector<Value>& inputs) override {
    requireInputs(inputs, 1, "stdev");
    if (anyNa(inputs)) return std::nullopt;
    window_.push(*inputs[0]);
    if (!window_.full()) return std::nullopt;
    const double mean = windowSum(window_) / n_;
    double sq = 0.0;
    for (std::size_t i = 0; i < window_.size(); ++i) {
      const double d = window_.at(i) - mean;
      sq += d * d;
    }
    return std::sqrt(sq / n_);
  }

 private:
  double n_;
  Window window_;
};

class Extremum final : public Indicator {
 public:
  Extremum(std::size_t n, bool isMax, const char* name) : window_(n), isMax_(isMax), name_(name) {}
  Value update(const std::vector<Value>& inputs) override {
    requireInputs(inputs, 1, name_);
    if (anyNa(inputs)) return std::nullopt;
    window_.push(*inputs[0]);
    if (!window_.full()) return std::nullopt;
    double best = window_.at(0);
    for (std::size_t i = 1; i < window_.size(); ++i) {
      const double v = window_.at(i);
      best = isMax_ ? std::max(best, v) : std::min(best, v);
    }
    return best;
  }

 private:
  Window window_;
  bool isMax_;
  const char* name_;
};

class Change final : public Indicator {
 public:
  explicit Change(std::size_t n) : window_(n + 1) {}
  Value update(const std::vector<Value>& inputs) override {
    requireInputs(inputs, 1, "change");
    if (anyNa(inputs)) return std::nullopt;
    const double x = *inputs[0];
    window_.push(x);
    if (!window_.full()) return std::nullopt;
    return x - window_.at(0);
  }

 private:
  Window window_;
};

// crossover: a[1] <= b[1] и a > b; crossunder: a[1] >= b[1] и a < b. На первом баре - false.
class Cross final : public Indicator {
 public:
  Cross(bool over, const char* name) : over_(over), name_(name) {}
  Value update(const std::vector<Value>& inputs) override {
    requireInputs(inputs, 2, name_);
    if (anyNa(inputs)) return std::nullopt;
    const double a = *inputs[0];
    const double b = *inputs[1];
    bool result = false;
    if (hasPrev_) {
      result = over_ ? (prevA_ <= prevB_ && a > b) : (prevA_ >= prevB_ && a < b);
    }
    prevA_ = a;
    prevB_ = b;
    hasPrev_ = true;
    return result ? 1.0 : 0.0;
  }

 private:
  bool over_;
  const char* name_;
  bool hasPrev_ = false;
  double prevA_ = 0.0;
  double prevB_ = 0.0;
};

std::size_t periodArg(std::string_view name, const std::vector<std::int64_t>& args) {
  if (args.size() != 1) {
    throw std::invalid_argument("индикатор " + std::string(name) +
                                ": ожидается один целый аргумент n");
  }
  if (args[0] < 1) {
    throw std::invalid_argument("индикатор " + std::string(name) + ": n должно быть не меньше 1");
  }
  return static_cast<std::size_t>(args[0]);
}

}  // namespace

std::unique_ptr<Indicator> makeIndicator(std::string_view name,
                                         const std::vector<std::int64_t>& intArgs) {
  if (name == "crossover" || name == "crossunder") {
    if (!intArgs.empty()) {
      throw std::invalid_argument("индикатор " + std::string(name) + ": целых аргументов нет");
    }
    const bool over = name == "crossover";
    return std::make_unique<Cross>(over, over ? "crossover" : "crossunder");
  }
  const bool known = name == "sma" || name == "ema" || name == "rsi" || name == "atr" ||
                     name == "stdev" || name == "highest" || name == "lowest" || name == "change";
  if (!known) throw std::invalid_argument("неизвестный индикатор: " + std::string(name));
  const std::size_t n = periodArg(name, intArgs);
  if (name == "sma") return std::make_unique<Sma>(n);
  if (name == "ema") return std::make_unique<Ema>(n);
  if (name == "rsi") return std::make_unique<Rsi>(n);
  if (name == "atr") return std::make_unique<Atr>(n);
  if (name == "stdev") return std::make_unique<Stdev>(n);
  if (name == "highest") return std::make_unique<Extremum>(n, true, "highest");
  if (name == "lowest") return std::make_unique<Extremum>(n, false, "lowest");
  return std::make_unique<Change>(n);
}

}  // namespace core::dsl
