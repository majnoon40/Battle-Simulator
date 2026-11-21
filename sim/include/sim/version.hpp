#pragma once

namespace sim {

// Simulation version (M10 Part 2): recorded into replays and battle
// reports. A replay written by a different version is refused by
// --replay. Bump this whenever simulation-visible state or rules change
// in a way old replays would not reproduce (terrain, morale, tactics,
// events, ...). Format: major.minor where major = milestone.
// 13.2 (m15): --flee-clamp on (default): every pending move clamps to
// the battlefield at the move-apply choke point (the S1 finding —
// routed units fled past the border); off reproduces 13.1
// byte-for-byte and rides in replay setups. This bump also closes the
// m14 gap: the item-0 counters change (v2-only behavior for listed
// types) shipped while the version stayed 13.1, so same-version
// pre-counters replays failed at checkpoint verify instead of a clean
// version refusal — from 13.2 they are refused up front.
// 13.1 (m13 item 3): --combat v2 (facing with flank/rear damage and
// rear morale loss, cavalry charges, fatigue, experience, commander
// auras, high-cohesion Hold defensive stance; data/combat.json). v1 is
// the exact 13.0 combat; the flag rides in replay setups. Combat depth
// never runs in legacy mode (M1 stays M1).
// 13.0 (m13 item 2): --ai-avail v2 (unavailable actions stay excluded —
// the regroup term no longer lifts Advance; MoveTo keeps its march
// explicitly; Help is capped per ally; stale contact estimates decay)
// and --end v2 (a faction below max(5, 5% of initial) active for 60
// ticks is routed). Both default to v2; v1 reproduces 11.2
// byte-for-byte and is recorded in replay setups.
inline constexpr const char* kSimVersion = "13.2";

}  // namespace sim
