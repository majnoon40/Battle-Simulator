// M4 acceptance harness: three report-only modes over the current sim.
//
//   archetypes <seed>            First unit ids per personality archetype
//                                (for picking --ai-debug targets).
//   attack-distance              Mixed scenario, seed 42, utility mode:
//                                average decide-time distance to target
//                                while in Attack stance, per unit type
//                                (archers should hold at range 8, infantry
//                                close to range 2).
//   battles <count=100>          500-unit utility battles, seeds 1..count:
//                                winner/tick/survivor distribution and
//                                per-faction action totals.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "sim/config.hpp"
#include "sim/personalities.hpp"
#include "sim/simulation.hpp"
#include "sim/unit_types.hpp"

namespace {

sim::UnitStats stats_for(const char* type) {
  sim::UnitStats stats;
  if (type == std::string("archer")) {
    stats.max_health = 70.0;
    stats.speed = 1.6;
    stats.damage = 9.0;
    stats.attack_range = 8.0;
    stats.attack_cooldown_ticks = 8;
    stats.sight_radius = 60.0;
  } else if (type == std::string("cavalry")) {
    stats.max_health = 160.0;
    stats.speed = 4.5;
    stats.damage = 20.0;
    stats.attack_range = 2.0;
    stats.attack_cooldown_ticks = 7;
    stats.sight_radius = 50.0;
  } else {
    stats.max_health = 100.0;
    stats.speed = 2.0;
    stats.damage = 12.0;
    stats.attack_range = 2.0;
    stats.attack_cooldown_ticks = 5;
    stats.sight_radius = 40.0;
  }
  return stats;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "battles";
  const sim::PersonalityTable& table = sim::default_personality_table();

  if (mode == "archetypes") {
    const std::uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 42;
    std::printf("first unit ids per archetype (seed %llu, default table):\n",
                static_cast<unsigned long long>(seed));
    for (std::size_t a = 0; a < table.size(); ++a) {
      std::printf("  %-10s:", table.archetypes[a].id.c_str());
      int found = 0;
      for (std::uint32_t id = 1; id <= 500 && found < 4; ++id) {
        const sim::Personality p = sim::personality_for(seed, id, table);
        if (static_cast<std::size_t>(p.archetype) == a) {
          std::printf(" %u", id);
          ++found;
        }
      }
      std::printf("\n");
    }
    return 0;
  }

  if (mode == "attack-distance") {
    // Mixed armies like data/scenarios/mixed.json: 100 infantry, 100 archers,
    // 50 cavalry per faction, on the default 1000x1000 battlefield.
    const sim::UnitTypeTable types = sim::parse_unit_types(R"json([
      {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0, "range": 2.0, "cooldown": 5, "sight_radius": 40.0},
      {"id": "archer", "health": 70.0, "damage": 9.0, "speed": 1.6, "range": 8.0, "cooldown": 8, "sight_radius": 60.0},
      {"id": "cavalry", "health": 160.0, "damage": 20.0, "speed": 4.5, "range": 2.0, "cooldown": 7, "sight_radius": 50.0}
    ])json");
    sim::SimulationConfig config;
    config.seed = 42;
    config.max_ticks = 3000;
    config.unit_types = &types;
    config.ai_mode = sim::AiMode::Utility;
    config.total_units = 500;
    const std::uint32_t infantry =
        static_cast<std::uint32_t>(types.index_of("infantry"));
    const std::uint32_t archer =
        static_cast<std::uint32_t>(types.index_of("archer"));
    const std::uint32_t cavalry =
        static_cast<std::uint32_t>(types.index_of("cavalry"));
    for (const auto [type, count] :
         std::vector<std::pair<std::uint32_t, std::uint32_t>>{
             {infantry, 100}, {archer, 100}, {cavalry, 50}}) {
      config.composition[static_cast<std::size_t>(sim::Faction::Red)]
          .push_back({type, count});
      config.composition[static_cast<std::size_t>(sim::Faction::Blue)]
          .push_back({type, count});
    }
    sim::Simulation simulation(config);

    // Per type: sum of decide-time distances on Attack-stance ticks.
    std::map<std::string, double> distance_sum;
    std::map<std::string, std::uint64_t> samples;
    std::uint64_t ticks = 0;
    while (!simulation.finished()) {
      simulation.tick();
      ++ticks;
      const sim::World& world = simulation.world();
      for (std::size_t i = 0; i < world.units.size(); ++i) {
        const sim::Unit& unit = world.units[i];
        if (!unit.alive ||
            static_cast<sim::Action>(unit.last_action) != sim::Action::Attack) {
          continue;
        }
        const sim::Perception& perception = simulation.perception(i);
        if (perception.nearest_visible_enemy < 0) {
          continue;
        }
        const std::string type_id =
            types.types[unit.type_index].id;
        distance_sum[type_id] += perception.nearest_visible_enemy_distance;
        samples[type_id] += 1;
      }
    }
    const sim::BattleResult result = simulation.result();
    std::printf("mixed scenario, seed 42, utility mode: %llu ticks, winner %s\n",
                static_cast<unsigned long long>(result.ticks_elapsed),
                result.has_winner ? sim::to_string(result.winner) : "draw");
    std::printf("average distance to target while attacking (decide time):\n");
    for (const auto& [type_id, sum] : distance_sum) {
      std::printf("  %-9s: %8.3f over %llu attack-ticks\n", type_id.c_str(),
                  sum / static_cast<double>(samples[type_id]),
                  static_cast<unsigned long long>(samples[type_id]));
    }
    return 0;
  }

  if (mode == "debug-find") {
    // Run the standard battle (seed 42, 500 infantry, morale on) and print
    // one (unit id, tick) pair where a unit breaks and one where a routed
    // unit recovers, for --ai-debug showcase runs.
    sim::SimulationConfig config;
    config.seed = 42;
    config.total_units = 500;
    config.max_ticks = 3000;
    config.ai_mode = sim::AiMode::Utility;
    config.morale_enabled = true;
    config.red_stats = stats_for("infantry");
    config.blue_stats = stats_for("infantry");
    sim::Simulation simulation(config);
    std::vector<bool> previous(simulation.world().units.size(), false);
    std::uint32_t break_unit = 0;
    std::uint64_t break_tick = 0;
    std::uint32_t recover_unit = 0;
    std::uint64_t recover_tick = 0;
    std::uint64_t tick = 0;
    while (!simulation.finished() && (break_unit == 0 || recover_unit == 0)) {
      simulation.tick();
      ++tick;
      for (std::size_t i = 0; i < simulation.world().units.size(); ++i) {
        const sim::Unit& unit = simulation.world().units[i];
        if (!unit.alive) {
          continue;
        }
        if (break_unit == 0 && unit.routed && !previous[i]) {
          break_unit = unit.id;
          break_tick = tick;
        }
        if (recover_unit == 0 && !unit.routed && previous[i]) {
          recover_unit = unit.id;
          recover_tick = tick;
        }
        previous[i] = unit.routed;
      }
    }
    const sim::BattleResult result = simulation.result();
    std::printf("seed 42 standard battle: %llu ticks\n",
                static_cast<unsigned long long>(result.ticks_elapsed));
    std::printf("break candidate: unit %u at tick %llu\n", break_unit,
                static_cast<unsigned long long>(break_tick));
    std::printf("recover candidate: unit %u at tick %llu (battle recoveries: Red %llu, Blue %llu)\n",
                recover_unit, static_cast<unsigned long long>(recover_tick),
                static_cast<unsigned long long>(
                    result.factions[0].routing.recoveries),
                static_cast<unsigned long long>(
                    result.factions[1].routing.recoveries));
    return 0;
  }

  if (mode == "morale-battles" || mode == "mismatch") {
    // Morale-era acceptance modes (M5). "morale-battles": equal 500-infantry
    // armies, seeds 1..count, morale on — winner/termination/tick/survivor
    // distribution and routing totals. "mismatch": a 3:1 army mismatch
    // (Red 375 vs Blue 125) — does the smaller army rout before it is
    // annihilated?
    const bool mismatch = mode == "mismatch";
    const int count = argc > 2 ? std::atoi(argv[2]) : 100;
    const std::uint32_t red_count = mismatch ? 375 : 250;
    const std::uint32_t blue_count = mismatch ? 125 : 250;
    const sim::UnitTypeTable types = sim::parse_unit_types(R"json([
      {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0, "range": 2.0, "cooldown": 5, "sight_radius": 40.0}
    ])json");
    std::uint32_t red_wins = 0;
    std::uint32_t blue_wins = 0;
    std::uint32_t draws = 0;
    std::uint32_t by_reason[4] = {0, 0, 0, 0};  // none/eliminated/routed/limit
    std::uint64_t total_ticks = 0;
    std::uint64_t min_ticks = ~0ULL;
    std::uint64_t max_ticks = 0;
    std::uint64_t total_survivors = 0;
    std::uint64_t total_deaths = 0;
    std::uint64_t total_blue_routed_at_end = 0;
    std::uint64_t small_side_routed_before_annihilated = 0;
    std::uint64_t total_routs[2] = {0, 0};
    std::uint64_t total_recoveries[2] = {0, 0};
    std::uint64_t total_peak[2] = {0, 0};
    for (int battle = 1; battle <= count; ++battle) {
      sim::SimulationConfig config;
      config.seed = static_cast<std::uint64_t>(battle);
      config.max_ticks = 3000;
      config.unit_types = &types;
      config.ai_mode = sim::AiMode::Utility;
      config.morale_enabled = true;
      config.composition[static_cast<std::size_t>(sim::Faction::Red)] = {
          {0, red_count}};
      config.composition[static_cast<std::size_t>(sim::Faction::Blue)] = {
          {0, blue_count}};
      sim::Simulation simulation(config);
      const sim::BattleResult result = simulation.run();
      if (!result.has_winner) {
        ++draws;
      } else if (result.winner == sim::Faction::Red) {
        ++red_wins;
      } else {
        ++blue_wins;
      }
      switch (result.reason) {
        case sim::TerminationReason::FactionEliminated:
          by_reason[1] += 1;
          break;
        case sim::TerminationReason::FactionRouted:
          by_reason[2] += 1;
          break;
        case sim::TerminationReason::TickLimit:
          by_reason[3] += 1;
          break;
        default:
          break;
      }
      total_ticks += result.ticks_elapsed;
      min_ticks = std::min(min_ticks, result.ticks_elapsed);
      max_ticks = std::max(max_ticks, result.ticks_elapsed);
      total_survivors += result.factions[0].survivors +
                         result.factions[1].survivors;
      total_deaths += result.factions[0].deaths + result.factions[1].deaths;
      total_blue_routed_at_end +=
          result.factions[1].survivors - result.factions[1].active;
      // "Smaller army routed before it was annihilated": the battle ended
      // while Blue still had routed survivors (routing decided it, not
      // annihilation).
      const bool blue_routed_not_dead =
          result.factions[1].survivors > 0 &&
          result.factions[1].active == 0;
      if (mismatch && blue_routed_not_dead) {
        ++small_side_routed_before_annihilated;
      }
      for (const sim::Faction faction : sim::kAllFactions) {
        const auto& report = result.factions[static_cast<std::size_t>(faction)];
        const std::size_t f = static_cast<std::size_t>(faction);
        total_routs[f] += report.routing.total_routs;
        total_recoveries[f] += report.routing.recoveries;
        total_peak[f] += report.routing.peak_routed;
      }
    }
    std::printf("%d utility battles, morale on (Red %u vs Blue %u, seeds 1..%d):\n",
                count, red_count, blue_count, count);
    std::printf("  winners: Red %u, Blue %u, draws %u\n", red_wins, blue_wins,
                draws);
    std::printf("  termination: eliminated %u, routed %u, tick-limit %u\n",
                by_reason[1], by_reason[2], by_reason[3]);
    std::printf("  ticks: min %llu, avg %.1f, max %llu\n",
                static_cast<unsigned long long>(min_ticks),
                static_cast<double>(total_ticks) / count,
                static_cast<unsigned long long>(max_ticks));
    std::printf("  survivors per battle: avg %.1f; deaths per battle: avg %.1f\n",
                static_cast<double>(total_survivors) / count,
                static_cast<double>(total_deaths) / count);
    if (mismatch) {
      std::printf("  smaller army routed before annihilation: %u / %d (%.0f%%)\n",
                  small_side_routed_before_annihilated, count,
                  100.0 * small_side_routed_before_annihilated / count);
    }
    std::printf("  Blue routed-at-end units per battle: avg %.1f\n",
                static_cast<double>(total_blue_routed_at_end) / count);
    const char* names[2] = {"Red", "Blue"};
    for (int f = 0; f < 2; ++f) {
      std::printf("  %-4s routs %llu, recoveries %llu, avg peak routed %.1f\n",
                  names[f], static_cast<unsigned long long>(total_routs[f]),
                  static_cast<unsigned long long>(total_recoveries[f]),
                  static_cast<double>(total_peak[f]) / count);
    }
    return 0;
  }

  if (mode == "battles") {
    const int count = argc > 2 ? std::atoi(argv[2]) : 100;
    std::uint32_t red_wins = 0;
    std::uint32_t blue_wins = 0;
    std::uint32_t draws = 0;
    std::uint64_t total_ticks = 0;
    std::uint64_t min_ticks = ~0ULL;
    std::uint64_t max_ticks = 0;
    std::uint64_t total_survivors = 0;
    std::uint64_t total_deaths = 0;
    std::array<std::array<std::uint64_t, sim::kActionCount>, 2> actions{};
    for (int battle = 1; battle <= count; ++battle) {
      sim::SimulationConfig config;
      config.seed = static_cast<std::uint64_t>(battle);
      config.total_units = 500;
      config.max_ticks = 3000;
      config.ai_mode = sim::AiMode::Utility;
      config.red_stats = stats_for("infantry");
      config.blue_stats = stats_for("infantry");
      sim::Simulation simulation(config);
      const sim::BattleResult result = simulation.run();
      if (!result.has_winner) {
        ++draws;
      } else if (result.winner == sim::Faction::Red) {
        ++red_wins;
      } else {
        ++blue_wins;
      }
      const std::uint64_t ticks = result.ticks_elapsed;
      total_ticks += ticks;
      min_ticks = std::min(min_ticks, ticks);
      max_ticks = std::max(max_ticks, ticks);
      total_survivors += result.factions[0].survivors +
                         result.factions[1].survivors;
      total_deaths += result.factions[0].deaths + result.factions[1].deaths;
      for (const sim::Faction faction : sim::kAllFactions) {
        const auto& report =
            result.factions[static_cast<std::size_t>(faction)];
        for (std::size_t a = 0; a < sim::kActionCount; ++a) {
          actions[static_cast<std::size_t>(faction)][a] += report.action_counts[a];
        }
      }
    }
    std::printf("%d utility battles (500 infantry, seeds 1..%d):\n", count, count);
    std::printf("  winners: Red %u, Blue %u, draws %u\n", red_wins, blue_wins,
                draws);
    std::printf("  ticks:   min %llu, avg %.1f, max %llu\n",
                static_cast<unsigned long long>(min_ticks),
                static_cast<double>(total_ticks) / count,
                static_cast<unsigned long long>(max_ticks));
    std::printf("  survivors per battle: avg %.1f; deaths per battle: avg %.1f\n",
                static_cast<double>(total_survivors) / count,
                static_cast<double>(total_deaths) / count);
    for (const sim::Faction faction : sim::kAllFactions) {
      const auto& per_faction =
          actions[static_cast<std::size_t>(faction)];
      std::printf("  %-4s action totals: attack %llu, approach %llu, advance %llu, retreat %llu, hold %llu\n",
                  sim::to_string(faction),
                  static_cast<unsigned long long>(
                      per_faction[static_cast<std::size_t>(sim::Action::Attack)]),
                  static_cast<unsigned long long>(
                      per_faction[static_cast<std::size_t>(sim::Action::Approach)]),
                  static_cast<unsigned long long>(
                      per_faction[static_cast<std::size_t>(sim::Action::Advance)]),
                  static_cast<unsigned long long>(
                      per_faction[static_cast<std::size_t>(sim::Action::Retreat)]),
                  static_cast<unsigned long long>(
                      per_faction[static_cast<std::size_t>(sim::Action::Hold)]));
    }
    return 0;
  }

  std::fprintf(stderr, "usage: ai_metrics archetypes|attack-distance|battles [n]\n");
  return 1;
}
