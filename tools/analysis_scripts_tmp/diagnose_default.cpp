// M10 STEP C diagnosis: on the DEFAULT 250v250 band-spawn scenario,
// variance on, 20 seeds: (1) the first tick any unit sees an enemy,
// (2) where each army sits at tick 600, (3) nearest-enemy spacing
// among alive actives at key ticks (why units pass each other unseen).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/unit_types.hpp"

namespace {

double dist(const sim::Unit& a, const sim::Unit& b) {
  return std::sqrt((a.x - b.x) * (a.x - b.x) +
                   (a.y - b.y) * (a.y - b.y));
}

}  // namespace

int main() {
  std::ifstream unit_file("data/units.json");
  const sim::UnitTypeTable types = sim::parse_unit_types(unit_file);
  std::ifstream scenario_file("data/scenarios/default.json");
  const sim::Scenario scenario = sim::parse_scenario(scenario_file);

  int first_seen_sum = 0;
  int first_seen_min = 1 << 30;
  int first_seen_max = 0;
  int seen_after_600 = 0;

  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    sim::SimulationConfig config;
    config.seed = seed;
    config.max_ticks = 3000;
    config.battlefield_width = scenario.battlefield_width;
    config.battlefield_height = scenario.battlefield_height;
    config.spawn_band_fraction = scenario.spawn_band_fraction;
    config.unit_types = &types;
    config.ai_mode = sim::AiMode::Utility;
    config.morale_enabled = true;
    config.composition = sim::build_composition(scenario, types, seed);
    sim::Simulation simulation(config);

    int first_seen = -1;
    const int probe_ticks[] = {200, 400, 600, 900, 1500, 2500};
    std::size_t probe = 0;
    std::printf("seed %2llu:", static_cast<unsigned long long>(seed));
    while (!simulation.finished()) {
      simulation.tick();
      const std::uint64_t tick = simulation.tick_index();
      if (first_seen < 0) {
        for (const sim::Unit& unit : simulation.world().units) {
          const int action = unit.last_action;
          // approach / attack / retreat all require a visible enemy.
          if (unit.alive && (action == 0 || action == 1 || action == 3)) {
            first_seen = static_cast<int>(tick);
            break;
          }
        }
      }
      if (probe < sizeof(probe_ticks) / sizeof(probe_ticks[0]) &&
          tick == static_cast<std::uint64_t>(probe_ticks[probe])) {
        // Army positions + spacing at this probe tick.
        double red_x = 0, red_y = 0, blue_x = 0, blue_y = 0;
        int red_n = 0, blue_n = 0;
        for (const sim::Unit& unit : simulation.world().units) {
          if (!unit.alive || unit.routed) {
            continue;
          }
          if (unit.faction == sim::Faction::Red) {
            red_x += unit.x; red_y += unit.y; ++red_n;
          } else {
            blue_x += unit.x; blue_y += unit.y; ++blue_n;
          }
        }
        if (red_n > 0 && blue_n > 0) {
          red_x /= red_n; red_y /= red_n;
          blue_x /= blue_n; blue_y /= blue_n;
          double nearest_sum = 0;
          int nearest_n = 0;
          double nearest_lt40 = 0;
          for (const sim::Unit& unit : simulation.world().units) {
            if (!unit.alive || unit.routed) {
              continue;
            }
            double best = std::numeric_limits<double>::max();
            for (const sim::Unit& other : simulation.world().units) {
              if (!other.alive || other.routed ||
                  other.faction == unit.faction) {
                continue;
              }
              best = std::min(best, dist(unit, other));
            }
            if (best < std::numeric_limits<double>::max()) {
              nearest_sum += best;
              ++nearest_n;
              if (best < 40.0) {
                ++nearest_lt40;
              }
            }
          }
          std::printf("  t%u R(%.0f,%.0f) B(%.0f,%.0f) dR-B %.0f "
                      "near-enemy mean %.0f in-sight %.0f%%",
                      probe_ticks[probe], red_x, red_y, blue_x, blue_y,
                      std::sqrt((red_x - blue_x) * (red_x - blue_x) +
                                (red_y - blue_y) * (red_y - blue_y)),
                      nearest_n > 0 ? nearest_sum / nearest_n : 0.0,
                      nearest_n > 0
                          ? 100.0 * nearest_lt40 / nearest_n
                          : 0.0);
        }
        ++probe;
      }
    }
    std::printf("  first-seen %d end %llu\n", first_seen,
                static_cast<unsigned long long>(
                    simulation.result().ticks_elapsed));
    if (first_seen > 0) {
      first_seen_sum += first_seen;
      first_seen_min = std::min(first_seen_min, first_seen);
      first_seen_max = std::max(first_seen_max, first_seen);
      if (first_seen > 600) {
        ++seen_after_600;
      }
    }
  }
  std::printf("\nfirst enemy seen: mean %.0f min %d max %d; seeds where "
              "nobody saw an enemy before tick 600: %d/20\n",
              first_seen_sum / 20.0, first_seen_min, first_seen_max,
              seen_after_600);
  return 0;
}
