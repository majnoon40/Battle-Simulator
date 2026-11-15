#pragma once

#include <cstdint>

namespace sim {

// Deterministic SplitMix64 pseudorandom generator. The simulation only ever
// draws numbers from instances of this class; there is no global random
// state. Given the same seed, the output sequence is identical on every
// platform because it uses plain 64-bit integer arithmetic only.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next_u64() noexcept;

  // Uniform double in [0, 1) with 53 bits of precision.
  [[nodiscard]] double next_double() noexcept;

  // Uniform integer in [0, bound); bound must be > 0. Uses rejection
  // sampling on the high 32 bits so there is no modulo bias.
  [[nodiscard]] std::uint32_t next_uint32(std::uint32_t bound) noexcept;

  // Current internal state (M10 Part 2b replay: canonical state hashing).
  [[nodiscard]] std::uint64_t state() const noexcept { return state_; }

 private:
  std::uint64_t state_;
};

}  // namespace sim
