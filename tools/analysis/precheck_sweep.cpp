// M2 pre-check: 200-seed faction-balance sweep (report only; no code changes
// to sim/). For each seed 1..200, 500 units, 3000 tick limit:
//
//   A: normal spawn          — Red left band (draws 1..2n), Blue right band
//   B: sides swapped         — same spawned units, every x mirrored (W - x):
//                              Red right band, same ids and draw stream
//   D: positions swapped     — Red unit i and Blue unit i exchange positions:
//                              Red right band, positions from the second half
//                              of the draw stream
//
// Writes one line per seed to logs/precheck-sweep-200.txt and a summary to
// stdout. Faction labels are never changed: in B and D the army called Red
// simply spawns on the other side.
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "sim/config.hpp"
#include "sim/factions.hpp"
#include "sim/results.hpp"
#include "sim/simulation.hpp"
#include "sim/unit.hpp"

namespace {

char winner_char(const sim::BattleResult& result) {
  if (!result.has_winner) {
    return 'D';
  }
  return result.winner == sim::Faction::Red ? 'R' : 'B';
}

struct Counts {
  int red = 0;
  int blue = 0;
  int draw = 0;
};

void add(char winner, Counts& counts) {
  if (winner == 'R') {
    ++counts.red;
  } else if (winner == 'B') {
    ++counts.blue;
  } else {
    ++counts.draw;
  }
}

}  // namespace

int main() {
  Counts a_counts;
  Counts b_counts;
  Counts d_counts;
  int a_b_agree = 0;
  int a_b_both_decisive = 0;

  std::ofstream log("logs/precheck-sweep-200.txt");
  if (!log) {
    std::cerr << "error: cannot open logs/precheck-sweep-200.txt\n";
    return 1;
  }

  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    sim::SimulationConfig config;
    config.seed = seed;
    config.total_units = 500;
    config.max_ticks = 3000;

    // A: normal spawn (Red left band, Blue right band).
    sim::Simulation sim_a(config);
    const std::vector<sim::Unit> spawned = sim_a.world().units;
    const sim::BattleResult result_a = sim_a.run();
    const char winner_a = winner_char(result_a);

    // B: mirror every spawn position (x -> width - x): sides swapped.
    std::vector<sim::Unit> mirrored = spawned;
    for (sim::Unit& unit : mirrored) {
      unit.x = config.battlefield_width - unit.x;
    }
    sim::Simulation sim_b(config, mirrored);
    const sim::BattleResult result_b = sim_b.run();
    const char winner_b = winner_char(result_b);

    // D: exchange positions between Red unit i and Blue unit i by spawn
    // order (no mirroring).
    std::vector<sim::Unit> swapped = spawned;
    const std::uint32_t red_count = sim::split_armies(config.total_units).red;
    for (std::uint32_t i = 0; i < red_count; ++i) {
      const double rx = swapped[i].x;
      const double ry = swapped[i].y;
      swapped[i].x = swapped[red_count + i].x;
      swapped[i].y = swapped[red_count + i].y;
      swapped[red_count + i].x = rx;
      swapped[red_count + i].y = ry;
    }
    sim::Simulation sim_d(config, swapped);
    const sim::BattleResult result_d = sim_d.run();
    const char winner_d = winner_char(result_d);

    add(winner_a, a_counts);
    add(winner_b, b_counts);
    add(winner_d, d_counts);
    if (winner_a != 'D' && winner_b != 'D') {
      ++a_b_both_decisive;
      if (winner_a == winner_b) {
        ++a_b_agree;
      }
    }

    log << "seed " << seed << "  A=" << winner_a << "("
        << result_a.ticks_elapsed << ")  B=" << winner_b << "("
        << result_b.ticks_elapsed << ")  D=" << winner_d << "("
        << result_d.ticks_elapsed << ")\n";
    std::cout << "." << std::flush;
  }

  const std::string summary =
      "\nSUMMARY over seeds 1..200 (500 units, 3000 tick limit)\n"
      "A normal       : Red=" +
      std::to_string(a_counts.red) + " Blue=" + std::to_string(a_counts.blue) +
      " Draw=" + std::to_string(a_counts.draw) +
      "\n"
      "B sides swapped: Red=" +
      std::to_string(b_counts.red) + " Blue=" + std::to_string(b_counts.blue) +
      " Draw=" + std::to_string(b_counts.draw) +
      "\n"
      "D pos swapped  : Red=" +
      std::to_string(d_counts.red) + " Blue=" + std::to_string(d_counts.blue) +
      " Draw=" + std::to_string(d_counts.draw) +
      "\n"
      "A vs B same winner (both decisive): " +
      std::to_string(a_b_agree) + " / " + std::to_string(a_b_both_decisive) +
      "\n";
  std::cout << summary;
  log << summary;
  return 0;
}
