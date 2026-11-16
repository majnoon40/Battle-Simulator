#pragma once

namespace sim {

// Simulation version (M10 Part 2): recorded into replays and battle
// reports. A replay written by a different version is refused by
// --replay. Bump this whenever simulation-visible state or rules change
// in a way old replays would not reproduce (terrain, morale, tactics,
// events, ...). Format: major.minor where major = milestone.
// 13.0 (m13 item 2): --ai-avail v2 (unavailable actions stay excluded —
// the regroup term no longer lifts Advance; MoveTo keeps its march
// explicitly; Help is capped per ally; stale contact estimates decay)
// and --end v2 (a faction below max(5, 5% of initial) active for 60
// ticks is routed). Both default to v2; v1 reproduces 11.2
// byte-for-byte and is recorded in replay setups.
inline constexpr const char* kSimVersion = "13.0";

}  // namespace sim
