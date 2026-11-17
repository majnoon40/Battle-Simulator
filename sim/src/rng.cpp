#include "sim/rng.hpp"

namespace sim {

// SplitMix64 (Stella Austin, Melissa O'Neill public domain reference
// implementation): 64-bit state, one round of mixing per draw.
std::uint64_t Rng::next_u64() noexcept {
  state_ += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state_;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

double Rng::next_double() noexcept {
  // 53 random bits scaled into [0, 1); exact for every representable value.
  return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
}

std::uint32_t Rng::next_uint32(std::uint32_t bound) noexcept {
  // Draw from the high 32 bits and reject the tail of the range that would
  // otherwise bias a modulo reduction. A bound of 0 has no result to
  // return; the modulo (and the limit computation) would divide by zero.
  if (bound == 0) {
    return 0;
  }
  const std::uint64_t limit =
      (1ULL << 32) - ((1ULL << 32) % bound);
  std::uint64_t r = 0;
  do {
    r = next_u64() >> 32;
  } while (r >= limit);
  return static_cast<std::uint32_t>(r % bound);
}

}  // namespace sim
