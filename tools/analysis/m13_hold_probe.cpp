// M13 Item 0.3 — hold-arm diagnosis probe.
//
// Replicates the orders-study Hold arm (squads scenario, 250v250 + 10
// commanders per side, all Red squads Hold at tick 100) and measures, per
// tick, for every alive Red/Blue unit:
//   - is an enemy inside the unit's attack range ("engaged")?
//   - what action did the unit choose (attack / hold / approach / ...)?
// so we can answer: P(chosen = Attack | enemy in attack range) for the
// holding side vs the attacking side, and watch the rout cascade that
// turns the kill ratio into ~0.05.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "sim/events.hpp"
#include "sim/morale.hpp"
#include "sim/personalities.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/squads.hpp"
#include "sim/tactics.hpp"
#include "sim/unit_types.hpp"

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);

std::string read_text(const std::string& path) {
  std::ifstream file(path);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

struct TickStat {
  std::uint64_t engaged = 0;      // enemy within attack range
  std::uint64_t attack = 0;       // engaged AND chose Attack
  std::uint64_t engaged_hold = 0;
  std::uint64_t engaged_approach = 0;
  std::uint64_t engaged_advance = 0;
  std::uint64_t engaged_retreat = 0;
  std::uint64_t engaged_flee = 0;
  std::uint64_t routed = 0;       // alive but routed flag set
  std::uint64_t alive = 0;
  std::uint64_t dead = 0;
};

}  // namespace

int main(int argc, char** argv) {
  const int seed = argc > 1 ? std::atoi(argv[1]) : 7;
  const bool hold = argc > 2 && std::string(argv[2]) == "hold";
  const int hold_tick = argc > 3 ? std::atoi(argv[3]) : 100;

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

  sim::SimulationConfig config;
  config.seed = seed;
  config.max_ticks = 1500;
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
  config.events = nullptr;

  sim::Simulation battle(config);
  const auto& units = battle.world().units;
  const std::size_t n = units.size();

  TickStat red;
  TickStat blue;
  TickStat red_total;  // sums over ticks >= hold_tick
  TickStat blue_total;
  std::uint64_t last_report = 0;

  auto nearest_enemy_dist = [&](std::size_t i) {
    double best = 1e30;
    for (std::size_t j = 0; j < n; ++j) {
      if (j == i || !units[j].alive || units[j].faction == units[i].faction) {
        continue;
      }
      const double dx = units[j].x - units[i].x;
      const double dy = units[j].y - units[i].y;
      best = std::min(best, dx * dx + dy * dy);
    }
    return std::sqrt(best);
  };

  for (int tick = 1; tick <= 1500; ++tick) {
    if (hold && tick == hold_tick) {
      std::vector<std::uint32_t> all;
      for (const sim::Squad& s : battle.squads()) {
        if (s.faction == sim::Faction::Red) {
          all.push_back(s.id);
        }
      }
      battle.issue_order(sim::Faction::Red, all,
                         sim::PlayerOrderType::Hold);
    }
    battle.tick();
    red = TickStat{};
    blue = TickStat{};
    for (std::size_t i = 0; i < n; ++i) {
      const sim::Unit& u = units[i];
      TickStat& stat = u.faction == sim::Faction::Red ? red : blue;
      if (!u.alive) {
        ++stat.dead;
        continue;
      }
      ++stat.alive;
      if (u.routed) {
        ++stat.routed;
      }
      if (u.routed || u.last_action ==
                          static_cast<std::uint8_t>(sim::Action::Flee)) {
        // Routed/fleeing units cannot choose; count the flee split below
        // only when they still have an enemy in range.
      }
      const bool in_range =
          !u.routed &&
          nearest_enemy_dist(i) <= u.stats.attack_range;
      if (!in_range) {
        continue;
      }
      ++stat.engaged;
      switch (static_cast<sim::Action>(u.last_action)) {
        case sim::Action::Attack: ++stat.attack; break;
        case sim::Action::Hold: ++stat.engaged_hold; break;
        case sim::Action::Approach: ++stat.engaged_approach; break;
        case sim::Action::Advance: ++stat.engaged_advance; break;
        case sim::Action::Retreat: ++stat.engaged_retreat; break;
        case sim::Action::Flee: ++stat.engaged_flee; break;
        default: break;
      }
    }
    if (tick >= hold_tick) {
      red_total.engaged += red.engaged;
      red_total.attack += red.attack;
      red_total.engaged_hold += red.engaged_hold;
      red_total.engaged_approach += red.engaged_approach;
      red_total.engaged_advance += red.engaged_advance;
      red_total.engaged_retreat += red.engaged_retreat;
      red_total.engaged_flee += red.engaged_flee;
      red_total.routed += red.routed;
      blue_total.engaged += blue.engaged;
      blue_total.attack += blue.attack;
      blue_total.routed += blue.routed;
    }
    if (tick - last_report >= 50) {
      last_report = tick;
      std::cout << "t" << tick
                << " | red alive " << red.alive << " dead " << red.dead
                << " routed " << red.routed
                << " engaged " << red.engaged << " attacking " << red.attack
                << " | blue alive " << blue.alive << " dead " << blue.dead
                << " routed " << blue.routed
                << " engaged " << blue.engaged << " attacking " << blue.attack
                << "\n";
    }
  }

  const double red_rate = red_total.engaged
                              ? double(red_total.attack) / red_total.engaged
                              : 0.0;
  const double blue_rate = blue_total.engaged
                               ? double(blue_total.attack) / blue_total.engaged
                               : 0.0;
  std::cout << "\n=== aggregate (ticks " << hold_tick << "..1500, seed "
            << seed << (hold ? ", HOLD arm" : ", CONTROL") << ") ===\n";
  std::cout << "RED  engaged-unit-ticks " << red_total.engaged
            << " | attack-when-engaged " << red_total.attack << " ("
            << (100.0 * red_rate) << "%)\n";
  std::cout << "RED  engaged-but: hold " << red_total.engaged_hold
            << " approach " << red_total.engaged_approach << " advance "
            << red_total.engaged_advance << " retreat "
            << red_total.engaged_retreat << " flee " << red_total.engaged_flee
            << "\n";
  std::cout << "BLUE engaged-unit-ticks " << blue_total.engaged
            << " | attack-when-engaged " << blue_total.attack << " ("
            << (100.0 * blue_rate) << "%)\n";
  std::cout << "final: red dead " << red.dead << " blue dead " << blue.dead
            << " (kill ratio "
            << double(blue.dead) / std::max<double>(double(red.dead), 1.0)
            << ")\n";
  return 0;
}
