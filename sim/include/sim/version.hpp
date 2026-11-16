#pragma once

namespace sim {

// Simulation version (M10 Part 2): recorded into replays and battle
// reports. A replay written by a different version is refused by
// --replay. Bump this whenever simulation-visible state or rules change
// in a way old replays would not reproduce (terrain, morale, tactics,
// events, ...). Format: major.minor where major = milestone.
// 11.2 (m12 audit): march orders no longer boost Approach; stickiness is
// capped under delivered orders; point orders work on flat maps and hold
// at the arrival radius; collapse clears the order; flat-map point
// validation. No-order battles are unchanged byte-for-byte.
inline constexpr const char* kSimVersion = "11.2";

}  // namespace sim
