// M3 profiling harness (report only; no sim/ changes).
//
// Measures where tick time goes at a given scale, against the CURRENT sim:
//   Phase 1 — full tick loop (grid mode, the M3 default), with snapshots
//             captured every N ticks and an operator-new counter split into
//             construction / warm-up / steady-state windows.
//   Phase 2 — the brute-force nearest-enemy scan alone:
//             sim::nearest_enemy_index over every alive unit of every
//             captured snapshot, repeated, rotating through the states so
//             the measurement sees realistically varied, battle-like data.
//             (Skip with the 5th argument "noscan" — at 20,000 units this
//             phase alone takes ~40 s.)
//
// The armies are M1-default infantry spawned exactly like the seed spawner
// (Red left band, Blue right band, x then y per unit), built as an explicit
// initial state so the harness is not subject to the configured unit cap.
//
// Usage: profile_phases [units=5000] [ticks=600] [world=1000] [seed=42]
//                       [noscan]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sim/config.hpp"
#include "sim/neighbors.hpp"
#include "sim/rng.hpp"
#include "sim/simulation.hpp"
#include "sim/unit.hpp"

// ---------------------------------------------------------------------------
// Global allocation counter (same technique as the steady-state test).
// ---------------------------------------------------------------------------
namespace {
std::size_t g_allocations = 0;
}  // namespace

void* operator new(std::size_t size) {
  ++g_allocations;
  if (size == 0) {
    size = 1;
  }
  void* pointer = std::malloc(size);
  if (pointer == nullptr) {
    throw std::bad_alloc();
  }
  return pointer;
}

void* operator new[](std::size_t size) { return operator new(size); }

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
  std::free(pointer);
}

namespace {

double seconds_since(const std::chrono::steady_clock::time_point& start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
      .count();
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint32_t units =
      argc > 1 ? static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10))
               : 5000;
  const std::uint64_t ticks =
      argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 600;
  const double world_size =
      argc > 3 ? std::strtod(argv[3], nullptr) : 1000.0;
  const std::uint64_t seed =
      argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 42;
  const bool skip_scan = argc > 5 && std::string(argv[5]) == "noscan";

  sim::SimulationConfig config;
  config.seed = seed;
  config.max_ticks = ticks;
  config.battlefield_width = world_size;
  config.battlefield_height = world_size;

  const double band = world_size * config.spawn_band_fraction;
  const std::uint32_t red = (units + 1) / 2;
  const std::uint32_t blue = units - red;
  sim::Rng rng(seed);
  std::vector<sim::Unit> initial;
  initial.reserve(units);
  std::uint32_t next_id = 1;
  for (std::uint32_t i = 0; i < red; ++i) {
    sim::Unit unit;
    unit.id = next_id++;
    unit.faction = sim::Faction::Red;
    unit.x = rng.next_double() * band;
    unit.y = rng.next_double() * world_size;
    unit.health = config.red_stats.max_health;
    unit.stats = config.red_stats;
    unit.alive = true;
    initial.push_back(unit);
  }
  for (std::uint32_t i = 0; i < blue; ++i) {
    sim::Unit unit;
    unit.id = next_id++;
    unit.faction = sim::Faction::Blue;
    unit.x = world_size - band + rng.next_double() * band;
    unit.y = rng.next_double() * world_size;
    unit.health = config.blue_stats.max_health;
    unit.stats = config.blue_stats;
    unit.alive = true;
    initial.push_back(unit);
  }

  // Phase 1: the real tick loop (grid mode), counting allocations per window.
  const std::size_t allocations_before_construction = g_allocations;
  sim::Simulation simulation(config, initial);
  const std::size_t allocations_construction =
      g_allocations - allocations_before_construction;

  std::vector<std::vector<sim::Unit>> snapshots;
  const std::uint64_t snapshot_every = 25;
  const std::uint64_t warmup_ticks = 50;
  double tick_seconds = 0.0;
  std::uint64_t ran = 0;
  // Allocation counters sample around each tick() call, so the harness's
  // own snapshot copies never pollute the numbers.
  std::size_t allocations_in_ticks = 0;
  std::size_t allocations_in_warmup = 0;
  for (std::uint64_t t = 0; t < ticks && !simulation.finished(); ++t) {
    if (t % snapshot_every == 0) {
      snapshots.push_back(simulation.world().units);  // not counted
    }
    const std::size_t allocations_before_tick = g_allocations;
    const auto start = std::chrono::steady_clock::now();
    simulation.tick();
    tick_seconds += seconds_since(start);
    const std::size_t tick_allocations =
        g_allocations - allocations_before_tick;
    allocations_in_ticks += tick_allocations;
    if (t < warmup_ticks) {
      allocations_in_warmup += tick_allocations;
    }
    ++ran;
  }
  snapshots.push_back(simulation.world().units);

  double scan_seconds = 0.0;
  std::uint64_t queries = 0;
  long sink = 0;
  const int scan_reps = 10;
  if (!skip_scan) {
    const auto scan_start = std::chrono::steady_clock::now();
    for (int rep = 0; rep < scan_reps; ++rep) {
      for (const std::vector<sim::Unit>& state : snapshots) {
        for (const sim::Unit& unit : state) {
          if (!unit.alive) {
            continue;
          }
          sink += sim::nearest_enemy_index(state, unit);
          ++queries;
        }
      }
    }
    scan_seconds = seconds_since(scan_start);
  }

  const double ms_per_tick = ran > 0 ? tick_seconds * 1000.0 / ran : 0.0;
  const double ticks_per_second =
      tick_seconds > 0.0 ? static_cast<double>(ran) / tick_seconds : 0.0;
  const double tick_equivalents =
      static_cast<double>(scan_reps) * static_cast<double>(snapshots.size());

  std::uint64_t alive_last = 0;
  for (const sim::Unit& unit : snapshots.back()) {
    if (unit.alive) {
      ++alive_last;
    }
  }

  std::printf(
      "== profile: units=%u ticks_limit=%llu world=%.0fx%.0f seed=%llu ==\n",
      units, static_cast<unsigned long long>(ticks), world_size, world_size,
      static_cast<unsigned long long>(seed));
  std::printf("ticks run:           %llu (finished: %s, %llu alive at end)\n",
              static_cast<unsigned long long>(ran),
              simulation.finished() ? "yes" : "no",
              static_cast<unsigned long long>(alive_last));
  std::printf("grid tick time:      %.3f s (%.3f ms/tick, %.1f ticks/s)\n",
              tick_seconds, ms_per_tick, ticks_per_second);
  std::printf("allocations:         %zu in construction, %zu in ticks 0..%llu (warm-up), %zu in ticks %llu..%llu (measured ticks total %llu)\n",
              allocations_construction, allocations_in_warmup,
              static_cast<unsigned long long>(
                  ran < warmup_ticks ? ran : warmup_ticks),
              allocations_in_ticks - allocations_in_warmup,
              static_cast<unsigned long long>(
                  ran < warmup_ticks ? ran : warmup_ticks),
              static_cast<unsigned long long>(ran),
              static_cast<unsigned long long>(ran));
  if (!skip_scan) {
    const double scan_ms_per_tick =
        tick_equivalents > 0.0 ? scan_seconds * 1000.0 / tick_equivalents : 0.0;
    std::printf("brute scan queries:  %llu over %.0f tick-equivalents\n",
                static_cast<unsigned long long>(queries), tick_equivalents);
    std::printf("brute scan per tick: %.3f ms (%.1fx the grid tick)\n",
                scan_ms_per_tick,
                ms_per_tick > 0.0 ? scan_ms_per_tick / ms_per_tick : 0.0);
  }
  std::printf("(sink=%ld)\n", sink);
  return 0;
}
