#pragma once

#include <string>

#include "sim/simulation.hpp"

namespace cli {

// Navigation tracing (m11a): one JSON object per --trace-interval ticks,
// written by battle_cli's --trace FILE. Pure read-only view of the world,
// so a traced battle is bit-identical to an untraced one (the trace opens
// no sim code paths and the string is built only at trace ticks).
//
// Line shape (one line, one JSON object):
//   {"tick":T,
//    "red":{"alive":A,"cx":x,"cy":y,"moved":m,"still":s,"advance":adv,
//           "xhist":[20 ints]},
//    "blue":{...}}
//
//   cx/cy   mean position of the faction's alive units
//   moved   mean |last move| of alive units this tick (world units)
//   still   fraction of alive units that moved less than 0.05 (the
//           engine's stuck epsilon) this tick
//   advance fraction whose last action was Advance
//   xhist   alive-unit counts in 20 equal-width x-buckets (a pile-up
//           shows as one or two dominant buckets that never shift)
//
// Intended for diagnosis of the m11a swap-orientation stalls and for the
// acceptance sweeps (army positions over time, stuck fractions).
[[nodiscard]] std::string trace_line(const sim::Simulation& simulation);

}  // namespace cli
