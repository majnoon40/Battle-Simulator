#pragma once

namespace sim {

// Simulation version (M10 Part 2): recorded into replays and battle
// reports. A replay written by a different version is refused by
// --replay. Bump this whenever simulation-visible state or rules change
// in a way old replays would not reproduce (terrain, morale, tactics,
// events, ...). Format: major.minor where major = milestone.
inline constexpr const char* kSimVersion = "10.2";

}  // namespace sim
