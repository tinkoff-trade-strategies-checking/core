#include "core/deterministic_run.hpp"

namespace core {
namespace {

// SplitMix64: простой генератор с полностью определённым поведением на любой платформе.
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

 private:
  std::uint64_t state_;
};

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

}  // namespace

RunResult run_stub(const RunConfig& config) {
  SplitMix64 rng(config.seed);
  RunResult result;
  result.series.reserve(config.steps);

  std::int64_t value = 0;
  std::uint64_t checksum = 14695981039346656037ULL;  // FNV-1a offset basis
  for (std::uint32_t step = 0; step < config.steps; ++step) {
    // Шаг в диапазоне [-100, 100]: условное «изменение капитала».
    value += static_cast<std::int64_t>(rng.next() % 201U) - 100;
    result.series.push_back(value);
    checksum ^= static_cast<std::uint64_t>(value);
    checksum *= 1099511628211ULL;  // FNV-1a prime
  }
  result.checksum = checksum;
  return result;
}

std::vector<std::uint8_t> serialize(const RunResult& result) {
  std::vector<std::uint8_t> out;
  out.reserve((result.series.size() + 2) * sizeof(std::uint64_t));
  append_u64(out, static_cast<std::uint64_t>(result.series.size()));
  for (const std::int64_t v : result.series) {
    append_u64(out, static_cast<std::uint64_t>(v));
  }
  append_u64(out, result.checksum);
  return out;
}

}  // namespace core
