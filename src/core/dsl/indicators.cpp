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
  explicit Window(std::size_t Slots) : Buf(Slots), Capacity(Slots) {}

  void push(double V) {
    Buf[Head] = V;
    Head = (Head + 1) % Capacity;
    if (Size < Capacity)
      ++Size;
  }
  bool full() const { return Size == Capacity; }
  // i = 0 — самое старое значение окна.
  double at(std::size_t I) const {
    const std::size_t Start = (Head + Capacity - Size) % Capacity;
    return Buf[(Start + I) % Capacity];
  }
  std::size_t size() const { return Size; }

private:
  std::vector<double> Buf;
  std::size_t Capacity;
  std::size_t Head = 0;
  std::size_t Size = 0;
};

void requireInputs(const std::vector<Value> &Inputs, std::size_t Expected,
                   const char *Name) {
  if (Inputs.size() != Expected) {
    throw std::invalid_argument(std::string("индикатор ") + Name +
                                ": ожидается входов " +
                                std::to_string(Expected) + ", получено " +
                                std::to_string(Inputs.size()));
  }
}

bool anyNa(const std::vector<Value> &Inputs) {
  return std::any_of(Inputs.begin(), Inputs.end(),
                     [](const Value &V) { return !V.has_value(); });
}

double windowSum(const Window &W) {
  double Sum = 0.0;
  for (std::size_t I = 0; I < W.size(); ++I)
    Sum += W.at(I);
  return Sum;
}

class Sma final : public Indicator {
public:
  explicit Sma(std::size_t Period)
      : N(static_cast<double>(Period)), Win(Period) {}
  Value update(const std::vector<Value> &Inputs) override {
    requireInputs(Inputs, 1, "sma");
    if (anyNa(Inputs))
      return std::nullopt;
    Win.push(*Inputs[0]);
    if (!Win.full())
      return std::nullopt;
    return windowSum(Win) / N;
  }

private:
  double N;
  Window Win;
};

class Ema final : public Indicator {
public:
  explicit Ema(std::size_t Period)
      : N(static_cast<double>(Period)),
        Alpha(2.0 / (static_cast<double>(Period) + 1.0)), Seed(Period) {}
  Value update(const std::vector<Value> &Inputs) override {
    requireInputs(Inputs, 1, "ema");
    if (anyNa(Inputs))
      return std::nullopt;
    const double X = *Inputs[0];
    if (!Seeded) {
      Seed.push(X);
      if (!Seed.full())
        return std::nullopt;
      Prev = windowSum(Seed) / N;
      Seeded = true;
      return Prev;
    }
    Prev = Alpha * X + (1.0 - Alpha) * Prev;
    return Prev;
  }

private:
  double N;
  double Alpha;
  Window Seed;
  bool Seeded = false;
  double Prev = 0.0;
};

// Сглаживание по Уайлдеру: первое значение — среднее первых n, дальше (prev *
// (n - 1) + x) / n.
class Wilder {
public:
  explicit Wilder(std::size_t Period)
      : N(static_cast<double>(Period)), Need(Period) {}
  // Возвращает сглаженное значение или nullopt, пока не набралось n значений.
  Value push(double X) {
    if (Count < Need) {
      Sum += X;
      ++Count;
      if (Count < Need)
        return std::nullopt;
      Avg = Sum / N;
      return Avg;
    }
    Avg = (Avg * (N - 1.0) + X) / N;
    return Avg;
  }

private:
  double N;
  std::size_t Need;
  std::size_t Count = 0;
  double Sum = 0.0;
  double Avg = 0.0;
};

class Rsi final : public Indicator {
public:
  explicit Rsi(std::size_t N) : Gain(N), Loss(N) {}
  Value update(const std::vector<Value> &Inputs) override {
    requireInputs(Inputs, 1, "rsi");
    if (anyNa(Inputs))
      return std::nullopt;
    const double X = *Inputs[0];
    if (!HasPrev) {
      Prev = X;
      HasPrev = true;
      return std::nullopt;
    }
    const double D = X - Prev;
    Prev = X;
    const Value G = Gain.push(D > 0.0 ? D : 0.0);
    const Value L = Loss.push(D < 0.0 ? -D : 0.0);
    if (!G || !L)
      return std::nullopt;
    if (*L == 0.0)
      return *G == 0.0 ? 50.0 : 100.0;
    return 100.0 - 100.0 / (1.0 + *G / *L);
  }

private:
  Wilder Gain;
  Wilder Loss;
  bool HasPrev = false;
  double Prev = 0.0;
};

class Atr final : public Indicator {
public:
  explicit Atr(std::size_t N) : Smooth(N) {}
  Value update(const std::vector<Value> &Inputs) override {
    requireInputs(Inputs, 3, "atr");
    if (anyNa(Inputs))
      return std::nullopt;
    const double High = *Inputs[0];
    const double Low = *Inputs[1];
    const double Close = *Inputs[2];
    double Tr = High - Low;
    if (HasPrevClose) {
      Tr = std::max(Tr, std::max(std::fabs(High - PrevClose),
                                 std::fabs(Low - PrevClose)));
    }
    PrevClose = Close;
    HasPrevClose = true;
    return Smooth.push(Tr);
  }

private:
  Wilder Smooth;
  bool HasPrevClose = false;
  double PrevClose = 0.0;
};

class Stdev final : public Indicator {
public:
  explicit Stdev(std::size_t Period)
      : N(static_cast<double>(Period)), Win(Period) {}
  Value update(const std::vector<Value> &Inputs) override {
    requireInputs(Inputs, 1, "stdev");
    if (anyNa(Inputs))
      return std::nullopt;
    Win.push(*Inputs[0]);
    if (!Win.full())
      return std::nullopt;
    const double Mean = windowSum(Win) / N;
    double Sq = 0.0;
    for (std::size_t I = 0; I < Win.size(); ++I) {
      const double D = Win.at(I) - Mean;
      Sq += D * D;
    }
    return std::sqrt(Sq / N);
  }

private:
  double N;
  Window Win;
};

class Extremum final : public Indicator {
public:
  Extremum(std::size_t Period, bool TakeMax, const char *IndicatorName)
      : Win(Period), IsMax(TakeMax), Name(IndicatorName) {}
  Value update(const std::vector<Value> &Inputs) override {
    requireInputs(Inputs, 1, Name);
    if (anyNa(Inputs))
      return std::nullopt;
    Win.push(*Inputs[0]);
    if (!Win.full())
      return std::nullopt;
    double Best = Win.at(0);
    for (std::size_t I = 1; I < Win.size(); ++I) {
      const double V = Win.at(I);
      Best = IsMax ? std::max(Best, V) : std::min(Best, V);
    }
    return Best;
  }

private:
  Window Win;
  bool IsMax;
  const char *Name;
};

class Change final : public Indicator {
public:
  explicit Change(std::size_t N) : Win(N + 1) {}
  Value update(const std::vector<Value> &Inputs) override {
    requireInputs(Inputs, 1, "change");
    if (anyNa(Inputs))
      return std::nullopt;
    const double X = *Inputs[0];
    Win.push(X);
    if (!Win.full())
      return std::nullopt;
    return X - Win.at(0);
  }

private:
  Window Win;
};

// crossover: a[1] <= b[1] и a > b; crossunder: a[1] >= b[1] и a < b. На первом
// баре - false.
class Cross final : public Indicator {
public:
  Cross(bool CrossOver, const char *IndicatorName)
      : Over(CrossOver), Name(IndicatorName) {}
  Value update(const std::vector<Value> &Inputs) override {
    requireInputs(Inputs, 2, Name);
    if (anyNa(Inputs))
      return std::nullopt;
    const double A = *Inputs[0];
    const double B = *Inputs[1];
    bool Result = false;
    if (HasPrev) {
      Result = Over ? (PrevA <= PrevB && A > B) : (PrevA >= PrevB && A < B);
    }
    PrevA = A;
    PrevB = B;
    HasPrev = true;
    return Result ? 1.0 : 0.0;
  }

private:
  bool Over;
  const char *Name;
  bool HasPrev = false;
  double PrevA = 0.0;
  double PrevB = 0.0;
};

std::size_t periodArg(std::string_view Name,
                      const std::vector<std::int64_t> &Args) {
  if (Args.size() != 1) {
    throw std::invalid_argument("индикатор " + std::string(Name) +
                                ": ожидается один целый аргумент n");
  }
  if (Args[0] < 1) {
    throw std::invalid_argument("индикатор " + std::string(Name) +
                                ": n должно быть не меньше 1");
  }
  return static_cast<std::size_t>(Args[0]);
}

} // namespace

std::unique_ptr<Indicator>
makeIndicator(std::string_view Name, const std::vector<std::int64_t> &IntArgs) {
  if (Name == "crossover" || Name == "crossunder") {
    if (!IntArgs.empty()) {
      throw std::invalid_argument("индикатор " + std::string(Name) +
                                  ": целых аргументов нет");
    }
    const bool Over = Name == "crossover";
    return std::make_unique<Cross>(Over, Over ? "crossover" : "crossunder");
  }
  const bool Known = Name == "sma" || Name == "ema" || Name == "rsi" ||
                     Name == "atr" || Name == "stdev" || Name == "highest" ||
                     Name == "lowest" || Name == "change";
  if (!Known)
    throw std::invalid_argument("неизвестный индикатор: " + std::string(Name));
  const std::size_t N = periodArg(Name, IntArgs);
  if (Name == "sma")
    return std::make_unique<Sma>(N);
  if (Name == "ema")
    return std::make_unique<Ema>(N);
  if (Name == "rsi")
    return std::make_unique<Rsi>(N);
  if (Name == "atr")
    return std::make_unique<Atr>(N);
  if (Name == "stdev")
    return std::make_unique<Stdev>(N);
  if (Name == "highest")
    return std::make_unique<Extremum>(N, true, "highest");
  if (Name == "lowest")
    return std::make_unique<Extremum>(N, false, "lowest");
  return std::make_unique<Change>(N);
}

} // namespace core::dsl
