#pragma once

#include <cmath>

namespace game {

// The available playback speeds, cycled by +/- in the window.
inline constexpr int kSpeeds[] = {1, 2, 4, 8, 16};
inline constexpr int kSpeedCount = 5;

// Fixed-timestep accumulator: converts variable frame times into a whole
// number of simulation ticks per frame at the current speed, with a per-
// frame cap so a slow frame can never freeze the window. Pure math; the
// window (and the tests) drive it with measured frame deltas.
class TickAccumulator {
 public:
  // `base_tps` is the simulation speed at 1x, in sim ticks per second.
  // Invalid parameters fall back to the defaults instead of poisoning the
  // clock: a non-finite or non-positive base_tps becomes 30.0, a
  // max_ticks_per_frame below 1 becomes 256 (a cap of 0 would freeze the
  // window, and both members must be finite before every comparison).
  explicit TickAccumulator(double base_tps = 30.0,
                           int max_ticks_per_frame = 256)
      : base_tps_(std::isfinite(base_tps) && base_tps > 0.0 ? base_tps : 30.0),
        max_ticks_per_frame_(max_ticks_per_frame >= 1 ? max_ticks_per_frame
                                                      : 256) {}

  // Sets the speed multiplier (>= 1).
  void set_speed(int speed) { speed_ = speed < 1 ? 1 : speed; }
  [[nodiscard]] int speed() const noexcept { return speed_; }

  // The speed multiplier after one step up or down through kSpeeds,
  // clamped at the ends (pure helper so window and tests agree).
  [[nodiscard]] static int speed_up(int speed) {
    for (int candidate : kSpeeds) {
      if (candidate > speed) {
        return candidate;
      }
    }
    return kSpeeds[kSpeedCount - 1];
  }
  [[nodiscard]] static int speed_down(int speed) {
    for (int i = kSpeedCount - 1; i >= 0; --i) {
      if (kSpeeds[i] < speed) {
        return kSpeeds[i];
      }
    }
    return kSpeeds[0];
  }

  // Advances the clock by `frame_dt_seconds` and returns how many ticks to
  // run this frame. The fractional remainder is kept for the next frame.
  // `limited` is set when the cap discarded ticks the frame wanted (the
  // window shows "sim limited" then, instead of freezing).
  //
  // Overflow safety: the accumulated double is never converted to int
  // before its range is checked — the cap comparison happens in double
  // space, so a huge (but finite) backlog or delta can never make the
  // conversion undefined. Non-finite frame deltas (NaN, +/-inf) add
  // nothing and report zero ticks; if the accumulated value itself ever
  // becomes non-finite (finite inputs whose product overflows), the
  // backlog is dropped (reset to zero), the cap is returned and
  // `limited` is set — one absurd frame must not freeze or poison every
  // later one.
  int advance(double frame_dt_seconds, bool& limited) {
    limited = false;
    if (!(frame_dt_seconds > 0.0)) {
      return 0;  // zero, negative and NaN deltas add nothing
    }
    if (!std::isfinite(frame_dt_seconds)) {
      return 0;  // +inf: a single unbounded frame adds no ticks either
    }
    carry_ += frame_dt_seconds * base_tps_ * static_cast<double>(speed_);
    if (!std::isfinite(carry_)) {
      carry_ = 0.0;
      limited = true;
      return max_ticks_per_frame_;
    }
    // Cap before conversion. The comparison reproduces the previous
    // `limited` semantics exactly — the flag meant "the truncated tick
    // count exceeded the cap", i.e. carry_ >= max + 1 — while never
    // converting an out-of-int-range double: the else branch only runs
    // for carry_ < max + 1 <= 2^31, where truncation is defined.
    int ticks = 0;
    if (carry_ >= static_cast<double>(max_ticks_per_frame_) + 1.0) {
      ticks = max_ticks_per_frame_;
      limited = true;
    } else {
      ticks = static_cast<int>(carry_);
    }
    carry_ -= static_cast<double>(ticks);
    if (carry_ >= 1.0) {
      limited = true;  // still behind after the cap
    }
    return ticks;
  }

  // Drops any pending fraction (used on restart / new seed).
  void reset() { carry_ = 0.0; }

  [[nodiscard]] double carry() const noexcept { return carry_; }

 private:
  double base_tps_;
  int max_ticks_per_frame_;
  int speed_ = 1;
  double carry_ = 0.0;
};

}  // namespace game
