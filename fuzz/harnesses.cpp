#include "fuzz/harnesses.hpp"

#include <cstdint>
#include <functional>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "cli/replay.hpp"
#include "game/settings.hpp"
#include "sim/combat.hpp"
#include "sim/squads.hpp"
#include "sim/morale.hpp"
#include "sim/personalities.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/tactics.hpp"
#include "sim/terrain.hpp"
#include "sim/unit_types.hpp"

namespace fuzz {
namespace {

// Every harness funnels through run(): a rejection is a return, never
// an escape. The catch-all keeps unknown exceptions from ending the
// fuzzing session before the interesting inputs arrive.
template <typename Fn>
int run(Fn&& fn) {
  try {
    fn();
  } catch (const std::exception&) {
    // rejected input — fine
  } catch (...) {
    // rejected input by a non-standard path — fine for the parsers;
    // a crash, a sanitizer report or a timeout is what we hunt.
  }
  return 0;
}

std::string as_text(const std::uint8_t* data, std::size_t size) {
  return std::string(reinterpret_cast<const char*>(data), size);
}

// The file-based parsers (replay, order script) get the bytes through
// a scratch file — that is exactly how untrusted input reaches them.
int with_scratch_file(const std::uint8_t* data, std::size_t size,
                      const std::string& suffix,
                      const std::function<void(const std::string&)>& fn) {
  const std::string path = "/tmp/bsfuzz_input" + suffix;
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data),
              static_cast<std::streamsize>(size));
  }
  int result = 0;
  try {
    fn(path);
  } catch (const std::exception&) {
  } catch (...) {
  }
  std::remove(path.c_str());
  return result;
}

// The simulation harness needs a unit table: the shipped one when it
// can be found (repo root or the build dir).
bool load_unit_table(sim::UnitTypeTable& types) {
  for (const char* path : {"data/units.json", "../data/units.json"}) {
    if (std::ifstream file(path); file) {
      const std::string text((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
      try {
        types = sim::parse_unit_types(text);
        return true;
      } catch (const std::exception&) {
        return false;
      }
    }
  }
  return false;
}

}  // namespace

int unit_types_harness(const std::uint8_t* data, std::size_t size) {
  return run([&] { static_cast<void>(sim::parse_unit_types(as_text(data, size))); });
}

int scenario_harness(const std::uint8_t* data, std::size_t size) {
  return run([&] { static_cast<void>(sim::parse_scenario(as_text(data, size))); });
}

int personalities_harness(const std::uint8_t* data, std::size_t size) {
  return run([&] { static_cast<void>(sim::parse_personalities(as_text(data, size))); });
}

int morale_harness(const std::uint8_t* data, std::size_t size) {
  return run([&] { static_cast<void>(sim::parse_morale(as_text(data, size))); });
}

int tactics_harness(const std::uint8_t* data, std::size_t size) {
  return run([&] { static_cast<void>(sim::parse_tactics(as_text(data, size))); });
}

int formations_harness(const std::uint8_t* data, std::size_t size) {
  return run([&] { static_cast<void>(sim::parse_formations(as_text(data, size))); });
}

int commanders_harness(const std::uint8_t* data, std::size_t size) {
  return run([&] { static_cast<void>(sim::parse_commanders(as_text(data, size))); });
}

int combat_harness(const std::uint8_t* data, std::size_t size) {
  return run([&] { static_cast<void>(sim::parse_combat(as_text(data, size))); });
}

int terrain_grid_harness(const std::uint8_t* data, std::size_t size) {
  return run([&] {
    static_cast<void>(sim::parse_terrain_grid_json(as_text(data, size),
                                                   2.0, nullptr));
  });
}

int settings_harness(const std::uint8_t* data, std::size_t size) {
  return run([&] {
    game::Settings settings;
    std::string error;
    static_cast<void>(game::parse_settings(as_text(data, size), settings,
                                           error));
  });
}

int replay_harness(const std::uint8_t* data, std::size_t size) {
  return with_scratch_file(
      data, size, ".replay", [](const std::string& path) {
        cli::ReplayData replay;
        std::string error;
        static_cast<void>(cli::parse_replay(path, replay, error));
      });
}

int order_script_harness(const std::uint8_t* data, std::size_t size) {
  return with_scratch_file(
      data, size, ".orders.json", [](const std::string& path) {
        std::vector<cli::OrderIntervention> orders;
        std::string error;
        static_cast<void>(cli::parse_order_script(path, orders, error));
      });
}

int simulation_harness(const std::uint8_t* data, std::size_t size) {
  return run([&] {
    const sim::Scenario scenario = sim::parse_scenario(as_text(data, size));
    sim::UnitTypeTable types;
    if (!load_unit_table(types)) {
      return;
    }
    // Bound the work: at most 1,000 units (the composition carries the
    // count entries) so the fuzzer explores the state machine, not the
    // 20k-unit benchmark.
    std::uint64_t total = 0;
    for (const auto& army : scenario.armies) {
      for (const sim::ScenarioEntry& entry : army) {
        total += entry.count_max > 0 ? entry.count_max : entry.count;
      }
    }
    if (total == 0 || total > 1000) {
      return;
    }
    const auto composition = sim::build_composition(scenario, types);
    sim::SimulationConfig config;
    config.seed = 42;
    config.max_ticks = 10;
    config.battlefield_width = scenario.battlefield_width;
    config.battlefield_height = scenario.battlefield_height;
    config.spawn_band_fraction = scenario.spawn_band_fraction;
    config.ai_mode = sim::AiMode::Utility;
    config.unit_types = &types;
    config.composition = composition;
    for (std::size_t faction = 0; faction < 2; ++faction) {
      if (const auto& plan = scenario.squads[faction]) {
        config.squad_plans[faction] = sim::SquadPlan{plan->count, plan->size};
      }
      if (const auto& anchor = scenario.anchors[faction]) {
        config.spawn_anchors[faction] = anchor;
      }
    }
    sim::Simulation simulation(config);
    for (int i = 0; i < 10 && !simulation.finished(); ++i) {
      simulation.tick();
    }
  });
}

}  // namespace fuzz
