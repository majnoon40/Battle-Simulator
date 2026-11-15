// M6 acceptance harness (report only): the three squad experiments.
//
//   cohesion [seed=42]     Standard squads battle (10 squads of 25 + 10
//                          commanders per faction, 1000x1000): mean distance
//                          of squad members to their squad centroid in the
//                          final state, squads ON vs OFF. For the OFF run
//                          the same deterministic assignment (taken from an
//                          identical ON construction at spawn) is applied to
//                          the final positions, so both numbers measure the
//                          same groups.
//   commander-kill [seeds=100] [tick=150]
//                          For each seed: a control battle and a treatment
//                          battle in which every living Blue commander-type
//                          unit is slayed right after the fixed tick (the
//                          deaths resolve in the next tick with full
//                          semantics: shock, command loss, succession).
//                          Reports paired differences in routs, ticks and
//                          outcomes.
//   equal [seeds=100]      Equal-army squads battles (260 v 260): how many
//                          end before the 3000-tick limit (target 80%).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sim/config.hpp"
#include "sim/simulation.hpp"
#include "sim/squads.hpp"
#include "sim/unit_types.hpp"

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);

// The three shipped squad types, mirroring data/units.json.
sim::UnitTypeTable load_types() {
  return sim::parse_unit_types(R"json([
    {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0,
     "range": 2.0, "cooldown": 5, "sight_radius": 40.0},
    {"id": "archer", "health": 70.0, "damage": 9.0, "speed": 1.6,
     "range": 8.0, "cooldown": 8, "sight_radius": 60.0},
    {"id": "cavalry", "health": 160.0, "damage": 20.0, "speed": 4.5,
     "range": 2.0, "cooldown": 7, "sight_radius": 50.0},
    {"id": "commander", "health": 150.0, "damage": 8.0, "speed": 1.8,
     "range": 2.0, "cooldown": 10, "sight_radius": 60.0,
     "leadership_radius": 120.0}
  ])json");
}

// The standard M6 squads battle: data/scenarios/squads.json in code form.
sim::SimulationConfig standard_battle(const sim::UnitTypeTable& types,
                                      std::uint64_t seed) {
  sim::SimulationConfig config;
  config.seed = seed;
  config.max_ticks = 3000;
  config.battlefield_width = 1000.0;
  config.battlefield_height = 1000.0;
  config.ai_mode = sim::AiMode::Utility;
  config.unit_types = &types;
  config.morale_enabled = true;
  const int infantry = types.index_of("infantry");
  const int commander = types.index_of("commander");
  for (const std::size_t faction : {kRed, kBlue}) {
    config.composition[faction].push_back(
        sim::TypeCount{static_cast<std::uint32_t>(commander), 10});
    config.composition[faction].push_back(
        sim::TypeCount{static_cast<std::uint32_t>(infantry), 250});
    config.squad_plans[faction] = sim::SquadPlan{10, 25};
  }
  return config;
}

// Mean distance of alive members to their squad's centroid, from the
// squad list of `assignment` (applied to `simulation`'s final positions).
double mean_member_distance(
    const sim::Simulation& simulation,
    const std::vector<sim::Squad>& assignment) {
  double total = 0.0;
  std::size_t members = 0;
  for (const sim::Squad& squad : assignment) {
    double sum_x = 0.0;
    double sum_y = 0.0;
    std::size_t alive = 0;
    for (const sim::SquadMemberState& member : squad.members) {
      const sim::Unit& unit = simulation.world().units[member.unit_index];
      if (unit.alive) {
        sum_x += unit.x;
        sum_y += unit.y;
        ++alive;
      }
    }
    if (alive == 0) {
      continue;
    }
    const double cx = sum_x / static_cast<double>(alive);
    const double cy = sum_y / static_cast<double>(alive);
    for (const sim::SquadMemberState& member : squad.members) {
      const sim::Unit& unit = simulation.world().units[member.unit_index];
      if (unit.alive) {
        total += std::sqrt((unit.x - cx) * (unit.x - cx) +
                           (unit.y - cy) * (unit.y - cy));
        ++members;
      }
    }
  }
  return members == 0 ? 0.0 : total / static_cast<double>(members);
}

int run_cohesion(int argc, char** argv) {
  const std::uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 42;
  const sim::UnitTypeTable types = load_types();

  // ON: the standard battle; OFF: the same battle with squads disabled.
  // The assignment comes from an identical ON construction (same spawn).
  sim::SimulationConfig on_config = standard_battle(types, seed);
  sim::Simulation on(on_config);
  sim::SimulationConfig off_config = standard_battle(types, seed);
  off_config.squads_enabled = false;
  sim::Simulation off(off_config);
  const std::vector<sim::Squad> assignment = on.squads();

  on.run();
  off.run();
  const double on_spread = mean_member_distance(on, on.squads());
  const double off_spread = mean_member_distance(off, assignment);
  std::printf("cohesion (seed %llu): squads ON mean distance to squad "
              "centroid %.3f, OFF %.3f, ratio %.3f\n",
              static_cast<unsigned long long>(seed), on_spread, off_spread,
              off_spread > 0.0 ? on_spread / off_spread : 0.0);
  std::printf("  ON: ticks %llu, reason %s, winner %s\n",
              static_cast<unsigned long long>(on.result().ticks_elapsed),
              sim::to_string(on.result().reason),
              on.result().has_winner ? sim::to_string(on.result().winner)
                                     : "draw");
  std::printf("  OFF: ticks %llu, reason %s, winner %s\n",
              static_cast<unsigned long long>(off.result().ticks_elapsed),
              sim::to_string(off.result().reason),
              off.result().has_winner ? sim::to_string(off.result().winner)
                                      : "draw");
  return 0;
}

int run_commander_kill(int argc, char** argv) {
  const int seeds = argc > 2 ? std::atoi(argv[2]) : 100;
  const std::uint64_t kill_tick = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 150;
  // Optional mechanism ablation: "no-succession" disables succession after
  // the kill (the squads stay leaderless for the rest of the battle).
  const bool no_succession = argc > 4 && std::string(argv[4]) == "no-succession";
  const sim::UnitTypeTable types = load_types();

  std::uint64_t control_ticks = 0, treatment_ticks = 0;
  std::uint64_t control_blue_routs = 0, treatment_blue_routs = 0;
  std::uint64_t control_red_routs = 0, treatment_red_routs = 0;
  int control_blue_wins = 0, treatment_blue_wins = 0;
  int control_red_wins = 0, treatment_red_wins = 0;
  int control_draws = 0, treatment_draws = 0;
  int treatment_flips = 0;  // outcome changed vs control
  int control_early = 0, treatment_early = 0;  // ended before tick limit
  int treatment_blue_routed = 0, control_blue_routed = 0;

  for (int seed = 1; seed <= seeds; ++seed) {
    // Control.
    sim::Simulation control(standard_battle(types, static_cast<std::uint64_t>(seed)));
    control.run();
    // Treatment: slay every living Blue commander-type unit right after
    // tick `kill_tick`; the deaths resolve next tick.
    sim::SimulationConfig treatment_config =
        standard_battle(types, static_cast<std::uint64_t>(seed));
    sim::CommanderConfig ablation_commanders;
    if (no_succession) {
      ablation_commanders = sim::default_commander_config();
      ablation_commanders.succession_delay_ticks = 100000;
      treatment_config.commanders = &ablation_commanders;
    }
    sim::Simulation treatment(treatment_config);
    while (!treatment.finished() &&
           treatment.tick_index() < kill_tick) {
      treatment.tick();
    }
    for (std::size_t i = 0; i < treatment.world().units.size(); ++i) {
      const sim::Unit& unit = treatment.world().units[i];
      if (unit.alive && unit.faction == sim::Faction::Blue &&
          unit.stats.leadership_radius > 0.0) {
        treatment.slay_unit(i);
      }
    }
    treatment.run();

    const sim::BattleResult& c = control.result();
    const sim::BattleResult& t = treatment.result();
    control_ticks += c.ticks_elapsed;
    treatment_ticks += t.ticks_elapsed;
    control_blue_routs += c.factions[kBlue].routing.total_routs;
    treatment_blue_routs += t.factions[kBlue].routing.total_routs;
    control_red_routs += c.factions[kRed].routing.total_routs;
    treatment_red_routs += t.factions[kRed].routing.total_routs;
    const int c_win = c.has_winner ? (c.winner == sim::Faction::Blue ? 1 : 2) : 0;
    const int t_win = t.has_winner ? (t.winner == sim::Faction::Blue ? 1 : 2) : 0;
    control_blue_wins += c_win == 1;
    control_red_wins += c_win == 2;
    control_draws += c_win == 0;
    treatment_blue_wins += t_win == 1;
    treatment_red_wins += t_win == 2;
    treatment_draws += t_win == 0;
    treatment_flips += c_win != t_win;
    control_early += c.reason != sim::TerminationReason::TickLimit;
    treatment_early += t.reason != sim::TerminationReason::TickLimit;
    control_blue_routed +=
        c.reason == sim::TerminationReason::FactionRouted &&
        c.has_winner && c.winner == sim::Faction::Red;
    treatment_blue_routed +=
        t.reason == sim::TerminationReason::FactionRouted &&
        t.has_winner && t.winner == sim::Faction::Red;
  }

  const double n = static_cast<double>(seeds);
  std::printf("commander-kill (%d seeds, Blue commanders slayed after tick "
              "%llu%s):\n",
              seeds, static_cast<unsigned long long>(kill_tick),
              no_succession ? ", succession disabled" : "");
  std::printf("  control:   avg ticks %.1f | Blue routs %.1f, Red routs %.1f"
              " | Red wins %d, Blue wins %d, draws %d | Blue routed out %d,"
              " early ends %d\n",
              static_cast<double>(control_ticks) / n,
              static_cast<double>(control_blue_routs) / n,
              static_cast<double>(control_red_routs) / n, control_red_wins,
              control_blue_wins, control_draws, control_blue_routed,
              control_early);
  std::printf("  treatment: avg ticks %.1f | Blue routs %.1f, Red routs %.1f"
              " | Red wins %d, Blue wins %d, draws %d | Blue routed out %d,"
              " early ends %d\n",
              static_cast<double>(treatment_ticks) / n,
              static_cast<double>(treatment_blue_routs) / n,
              static_cast<double>(treatment_red_routs) / n, treatment_red_wins,
              treatment_blue_wins, treatment_draws, treatment_blue_routed,
              treatment_early);
  std::printf("  paired: Blue routs %+.1f per battle, ticks %+.1f, outcomes"
              " flipped in %d/%d battles\n",
              (static_cast<double>(treatment_blue_routs) -
               static_cast<double>(control_blue_routs)) / n,
              (static_cast<double>(treatment_ticks) -
               static_cast<double>(control_ticks)) / n,
              treatment_flips, seeds);
  return 0;
}

int run_equal(int argc, char** argv) {
  const int seeds = argc > 2 ? std::atoi(argv[2]) : 100;
  const sim::UnitTypeTable types = load_types();
  int early = 0;
  int routed = 0;
  int eliminated = 0;
  std::uint64_t tick_sum = 0;
  int red_wins = 0, blue_wins = 0, draws = 0;
  std::uint64_t rout_sum = 0;
  for (int seed = 1; seed <= seeds; ++seed) {
    sim::Simulation simulation(
        standard_battle(types, static_cast<std::uint64_t>(seed)));
    const sim::BattleResult result = simulation.run();
    tick_sum += result.ticks_elapsed;
    rout_sum += result.factions[kRed].routing.total_routs +
                result.factions[kBlue].routing.total_routs;
    if (result.reason != sim::TerminationReason::TickLimit) {
      ++early;
      if (result.reason == sim::TerminationReason::FactionRouted) {
        ++routed;
      } else {
        ++eliminated;
      }
    }
    if (result.has_winner) {
      result.winner == sim::Faction::Red ? ++red_wins : ++blue_wins;
    } else {
      ++draws;
    }
  }
  const double n = static_cast<double>(seeds);
  std::printf("equal armies (%d seeds, 260 v 260, squads on, morale on, "
              "tick limit 3000):\n",
              seeds);
  std::printf("  ended before the tick limit: %d/%d (%.0f%%, target 80%%)"
              " [faction_routed %d, eliminated %d]\n",
              early, seeds, 100.0 * early / n, routed, eliminated);
  std::printf("  avg ticks %.1f, avg total routs %.1f, Red wins %d, Blue"
              " wins %d, draws %d\n",
              static_cast<double>(tick_sum) / n,
              static_cast<double>(rout_sum) / n, red_wins, blue_wins, draws);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: m6_experiments <cohesion|commander-kill|equal> "
                 "[args]\n");
    return 1;
  }
  const std::string mode = argv[1];
  if (mode == "cohesion") {
    return run_cohesion(argc, argv);
  }
  if (mode == "commander-kill") {
    return run_commander_kill(argc, argv);
  }
  if (mode == "equal") {
    return run_equal(argc, argv);
  }
  std::fprintf(stderr, "unknown mode '%s'\n", mode.c_str());
  return 1;
}
