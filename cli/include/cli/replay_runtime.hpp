#pragma once

// m13 item 5b — the shared replay tick driver. The record / replay /
// seek / verify loop semantics (apply due interventions and orders
// between ticks, hash every checkpoint tick, append on record, compare
// on verify, stop at the seek target) were inline in the CLI's run
// function; the window's replay viewer needs the exact same behavior,
// so the loop lives here once. The CLI and the window both drive it;
// tests pin it headlessly.
//
// Checkpoint ticks (the ticks a state hash covers) are exactly the
// CLI's rule: every 25th tick, the battle's final tick, and the seek
// target when one is given.

#include <cstdint>
#include <string>
#include <vector>

#include "cli/cli_args.hpp"
#include "cli/replay.hpp"
#include "sim/simulation.hpp"

namespace cli {

// Maps a parsed replay's setup onto the CLI arguments that reproduce
// it (scenario path, seed, tick limit, every recorded flag). The CLI's
// --replay and the window's replay viewer (m13 item 5b) both build
// their simulation config from this so a replayed battle is set up
// exactly the way it was recorded.
[[nodiscard]] cli::Arguments arguments_from_replay(
    const ReplayData& replay);

class ReplayDriver {
 public:
  // `interventions` and `orders` must be sorted ascending by tick (the
  // parse and record paths guarantee it; the driver consumes them in
  // order).
  ReplayDriver(sim::Simulation& simulation,
               const std::vector<Intervention>& interventions,
               const std::vector<OrderIntervention>& orders);

  struct RunResult {
    std::uint64_t last_tick = 0;   // ticks completed when the run stopped
    bool mismatch = false;         // verify: a checkpoint hash differed
    std::uint64_t mismatch_tick = 0;
    std::string expected;          // the recorded hash at the mismatch
    std::string got;               // the re-run hash at the mismatch
    std::size_t checkpoints_verified = 0;  // consumed recorded hashes
  };

  // Runs the battle one tick: applies every intervention and order due
  // at the upcoming tick (ascending), advances, and returns the state
  // hash checkpoint decision to the caller through run() — step() alone
  // never hashes (the CLI's one-tick stepping and the window's scrub
  // share it).
  void step(std::vector<std::string>* warnings);

  // Runs until `target_tick` (0 = until the battle finishes), hashing
  // every checkpoint tick. `record` (optional) appends the checkpoints;
  // `verify` (optional) compares against its hashes and stops at the
  // first mismatch (filling mismatch_tick/expected/got). Seeking to a
  // target makes that tick a checkpoint, exactly like --seek.
  RunResult run(std::uint64_t target_tick, ReplayData* record,
                const ReplayData* verify,
                std::vector<std::string>* warnings = nullptr);

  [[nodiscard]] std::size_t interventions_applied() const noexcept {
    return next_intervention_;
  }
  [[nodiscard]] std::size_t orders_applied() const noexcept {
    return next_order_;
  }

 private:
  sim::Simulation& simulation_;
  const std::vector<Intervention>& interventions_;
  const std::vector<OrderIntervention>& orders_;
  std::size_t next_intervention_ = 0;
  std::size_t next_order_ = 0;
};

}  // namespace cli
