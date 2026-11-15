#pragma once

#include <cstdint>
#include <string>

namespace sim {

// Replay support (M10 Part 2b).
//
// state_hash() returns a canonical, byte-stable 128-bit FNV-1a digest
// (32 hex chars) of ALL simulation state that can influence the future
// of the battle: tick, RNG state, every per-unit field (position,
// health, morale, memory, pursuit, commit), perceptions, squad state,
// faction-level counters, estimates, stuck bookkeeping and objectives.
// The same state always hashes identically on the same build, so a
// recorded run and a re-run diverge iff the hashes diverge.
//
// The replay FILE format (written by --record, read by --replay) is a
// line-based text format:
//
//   BATTLESIM REPLAY 1
//   version 11.0
//   scenario <name>            (as passed to --scenario)
//   mod <dir or ->             (as passed to --mod, "-" = none)
//   seed <n>
//   ticks <n>                  (the tick limit used)
//   flags variance=on|off contact_seek=on|off neighbor=grid|brute
//         squads=on|off swap_sides=on|off nav=v1|v2
//         (nav is m11a; files without it parse as v1)
//   intervene kill <unit> <tick>     (0+ lines, ascending tick)
//   hash <tick> <32-hex>            (checkpoints, ascending tick)
//
// --replay refuses a file whose version differs from sim::kSimVersion.

// Appends the canonical byte serialization of the simulation state.
// (Declared here; implemented in simulation.cpp where the private
// members live.)
class Simulation;

}  // namespace sim
