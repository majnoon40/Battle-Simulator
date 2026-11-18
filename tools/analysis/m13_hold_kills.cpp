// M13 Item H(a) — hold-arm kill attribution and enemy action accounting.
//
// Replicates the orders-study Hold arm (squads scenario, 250v250 + 10
// commanders per side, all Red squads Hold at tick 100) over N seeds and
// records:
//   - every Red death: tick, killer unit type, killer<->victim distance
//     at the time of the kill (positions at end-of-tick == positions at
//     the kill: movement commits before combat in the same tick);
//   - Blue action totals per 100-tick bin, split so the "~88k Search
//     ticks" question gets a direct answer: Search is only scored when
//     the unit sees NO enemy (ai.cpp: Search requires !has_visible_enemy),
//     so "search while seeing the blob" should be structurally zero —
//     the bins count search_blind vs search_sighted to prove it, plus the
//     nearest-Red distance histogram of the Search choosers;
//   - per-seed outcomes (end tick, winner, deaths) for context.
//
// Usage: m13_hold_kills <first_seed> <seed_count> [hold_tick]
// Output: one per-seed summary line, then the aggregate tables.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "sim/events.hpp"
#include "sim/morale.hpp"
#include "sim/personalities.hpp"
#include "sim/results.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/squads.hpp"
#include "sim/tactics.hpp"
#include "sim/unit_types.hpp"

namespace {

std::string read_text(const std::string& path) {
  std::ifstream file(path);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

const char* kDistanceBands = "[0,2] (2,8] (8,30] (30,60] >60";
int distance_band(double d) {
  if (d <= 2.0) return 0;
  if (d <= 8.0) return 1;
  if (d <= 30.0) return 2;
  if (d <= 60.0) return 3;
  return 4;
}

// Blue action accounting, per 100-tick bin.
struct Bin {
  std::uint64_t search_blind = 0;    // chose Search, no Red in sight
  std::uint64_t search_sighted = 0;  // chose Search, Red in sight (bug!)
  std::uint64_t approach = 0;
  std::uint64_t attack = 0;
  std::uint64_t retreat = 0;
  std::uint64_t hold = 0;
  std::uint64_t flee = 0;
  std::uint64_t other = 0;
  std::uint64_t alive_blue = 0;
  // nearest-Red distance of Search choosers, same bands as kills
  std::uint64_t search_dist[5] = {0, 0, 0, 0, 0};
};

}  // namespace

int main(int argc, char** argv) {
  const int first_seed = argc > 1 ? std::atoi(argv[1]) : 1;
  const int seed_count = argc > 2 ? std::atoi(argv[2]) : 20;
  const int hold_tick = argc > 3 ? std::atoi(argv[3]) : 100;
  constexpr int kMaxTicks = 1500;
  constexpr int kBins = kMaxTicks / 100;

  const sim::UnitTypeTable types =
      sim::parse_unit_types(read_text("data/units.json"));
  const sim::PersonalityTable personalities =
      sim::parse_personalities(read_text("data/personalities.json"));
  const sim::FormationConfig formations =
      sim::parse_formations(read_text("data/formations.json"));
  const sim::CommanderConfig commanders =
      sim::parse_commanders(read_text("data/commanders.json"));
  const sim::MoraleConfig morale =
      sim::parse_morale(read_text("data/morale.json"));
  const sim::TacticsConfig tactics =
      sim::parse_tactics(read_text("data/tactics.json"));
  const sim::Scenario scenario =
      sim::parse_scenario(read_text("data/scenarios/squads.json"));

  // Aggregates over seeds.
  std::map<std::string, std::uint64_t> kills_by_killer_type;
  std::uint64_t kill_band[5] = {0, 0, 0, 0, 0};
  std::uint64_t kill_total = 0;
  Bin bins[kBins];
  std::uint64_t red_deaths_sum = 0;
  std::uint64_t blue_deaths_sum = 0;
  std::uint64_t end_tick_sum = 0;
  std::uint64_t red_wins = 0;

  for (int seed = first_seed; seed < first_seed + seed_count; ++seed) {
    sim::SimulationConfig config;
    config.seed = seed;
    config.max_ticks = kMaxTicks;
    config.ai_mode = sim::AiMode::Utility;
    config.morale_enabled = true;
    config.morale = &morale;
    config.personalities = &personalities;
    config.unit_types = &types;
    config.tactics = &tactics;
    config.tactics_enabled = true;
    config.battlefield_width = scenario.battlefield_width;
    config.battlefield_height = scenario.battlefield_height;
    config.spawn_band_fraction = scenario.spawn_band_fraction;
    config.squads_enabled = true;
    config.formations = &formations;
    config.commanders = &commanders;
    for (sim::Faction faction : sim::kAllFactions) {
      const std::size_t index = static_cast<std::size_t>(faction);
      if (const auto& plan = scenario.squads[index]) {
        config.squad_plans[index] = sim::SquadPlan{plan->count, plan->size};
      }
    }
    config.composition = sim::build_composition(scenario, types, seed);
    config.orders_enabled = true;

    sim::EventRecorder recorder;
    config.events = &recorder;
    sim::Simulation battle(config);
    const auto& world = battle.world();
    const auto& units = world.units;
    const std::size_t n = units.size();

    std::size_t processed_events = 0;
    std::uint64_t red_deaths = 0;
    std::uint64_t blue_deaths = 0;

    for (int tick = 1; tick <= kMaxTicks; ++tick) {
      if (tick == hold_tick) {
        std::vector<std::uint32_t> all;
        for (const sim::Squad& s : battle.squads()) {
          if (s.faction == sim::Faction::Red) {
            all.push_back(s.id);
          }
        }
        static_cast<void>(battle.issue_order(sim::Faction::Red, all,
                           sim::PlayerOrderType::Hold));
      }
      const std::uint64_t ticks_before = world.tick_count;
      battle.tick();
      if (world.tick_count == ticks_before) {
        break;  // terminated earlier (rout / stalemate / elimination)
      }

      // --- Blue action accounting for this tick ----------------------
      Bin& bin = bins[std::min((tick - 1) / 100, kBins - 1)];
      for (std::size_t i = 0; i < n; ++i) {
        const sim::Unit& u = units[i];
        if (!u.alive || u.faction != sim::Faction::Blue) {
          continue;
        }
        ++bin.alive_blue;
        const sim::Action action = static_cast<sim::Action>(u.last_action);
        if (action == sim::Action::Search) {
          // Does this unit see any alive Red (sight is a plain radius)?
          double nearest = 1e30;
          for (std::size_t j = 0; j < n; ++j) {
            const sim::Unit& r = units[j];
            if (!r.alive || r.faction != sim::Faction::Red) {
              continue;
            }
            const double dx = r.x - u.x;
            const double dy = r.y - u.y;
            const double d2 = dx * dx + dy * dy;
            if (d2 < nearest) {
              nearest = d2;
            }
          }
          const double d = std::sqrt(nearest);
          if (d <= u.stats.sight_radius) {
            ++bin.search_sighted;  // Search while seeing Red: AI bug
          } else {
            ++bin.search_blind;
          }
          ++bin.search_dist[distance_band(d)];
        } else if (action == sim::Action::Approach) {
          ++bin.approach;
        } else if (action == sim::Action::Attack) {
          ++bin.attack;
        } else if (action == sim::Action::Retreat) {
          ++bin.retreat;
        } else if (action == sim::Action::Hold) {
          ++bin.hold;
        } else if (action == sim::Action::Flee) {
          ++bin.flee;
        } else {
          ++bin.other;
        }
      }

      // --- deaths this tick: killer type + distance -------------------
      const auto& events = recorder.events();
      for (; processed_events < events.size(); ++processed_events) {
        const sim::Event& e = events[processed_events];
        if (e.type != sim::EventType::Death) {
          continue;
        }
        const sim::Unit* victim = world.find_by_id(e.a);
        if (victim == nullptr) {
          continue;
        }
        if (victim->faction == sim::Faction::Blue) {
          ++blue_deaths;
          continue;
        }
        ++red_deaths;
        ++kill_total;
        const sim::Unit* killer = e.b == 0 ? nullptr : world.find_by_id(e.b);
        if (killer == nullptr) {
          kills_by_killer_type["(no killer recorded)"] += 1;
          continue;
        }
        kills_by_killer_type[types.types[killer->type_index].id] += 1;
        const double dx = killer->x - victim->x;
        const double dy = killer->y - victim->y;
        ++kill_band[distance_band(std::sqrt(dx * dx + dy * dy))];
      }
    }

    std::uint64_t red_alive = 0;
    std::uint64_t blue_alive = 0;
    for (const sim::Unit& u : units) {
      if (!u.alive) continue;
      if (u.faction == sim::Faction::Red) ++red_alive; else ++blue_alive;
    }
    const bool red_win = blue_alive == 0 && red_alive > 0;
    if (red_win) ++red_wins;
    end_tick_sum += world.tick_count;
    red_deaths_sum += red_deaths;
    blue_deaths_sum += blue_deaths;
    std::printf(
        "seed %d: end t%llu (%s), red %llu dead / %llu alive, blue %llu dead, "
        "red win %d\n",
        seed, static_cast<unsigned long long>(world.tick_count),
        sim::to_string(battle.result().reason),
        static_cast<unsigned long long>(red_deaths),
        static_cast<unsigned long long>(red_alive),
        static_cast<unsigned long long>(blue_deaths), red_win ? 1 : 0);
  }

  std::printf("\n=== Item H(a) hold arm, %d seeds (ticks to %d) ===\n",
              seed_count, kMaxTicks);
  std::printf("mean end tick %.1f | red wins %d/%d | red dead/seed %.1f | "
              "blue dead/seed %.1f\n",
              double(end_tick_sum) / seed_count, static_cast<int>(red_wins),
              seed_count, double(red_deaths_sum) / seed_count,
              double(blue_deaths_sum) / seed_count);
  std::printf("\nRed deaths by killer type:\n");
  for (const auto& [type, count] : kills_by_killer_type) {
    std::printf("  %-16s %llu (%.1f%%)\n", type.c_str(),
                static_cast<unsigned long long>(count),
                100.0 * double(count) / double(kill_total));
  }
  std::printf("\nKill distance at time of kill (killer to victim), bands %s:\n",
              kDistanceBands);
  for (int b = 0; b < 5; ++b) {
    std::printf("  band %d: %llu (%.1f%%)\n", b,
                static_cast<unsigned long long>(kill_band[b]),
                100.0 * double(kill_band[b]) / double(kill_total));
  }
  std::printf("\nBlue action-unit-ticks per 100-tick bin (mean per seed):\n");
  std::printf("%7s %10s %12s %9s %9s %8s %8s %8s\n", "ticks",
              "search(b)", "search(vis)", "approach", "attack", "retreat",
              "hold", "flee");
  for (int b = 0; b < kBins; ++b) {
    std::printf("%4d-%3d %10.1f %12.1f %9.1f %9.1f %8.1f %8.1f %8.1f\n",
                b * 100 + 1, (b + 1) * 100,
                double(bins[b].search_blind) / seed_count,
                double(bins[b].search_sighted) / seed_count,
                double(bins[b].approach) / seed_count,
                double(bins[b].attack) / seed_count,
                double(bins[b].retreat) / seed_count,
                double(bins[b].hold) / seed_count,
                double(bins[b].flee) / seed_count);
  }
  std::uint64_t sb = 0, sv = 0;
  for (const Bin& b : bins) {
    sb += b.search_blind;
    sv += b.search_sighted;
  }
  std::printf("Search totals: blind %llu, sighted %llu (sighted must be 0: "
              "Search is only scored with no visible enemy)\n",
              static_cast<unsigned long long>(sb),
              static_cast<unsigned long long>(sv));
  std::printf("\nSearch chooser distance to nearest Red (bands %s):\n",
              kDistanceBands);
  for (int b = 0; b < 5; ++b) {
    std::uint64_t t = 0;
    for (const Bin& bin : bins) t += bin.search_dist[b];
    std::printf("  band %d: %llu\n", b, static_cast<unsigned long long>(t));
  }
  return 0;
}
