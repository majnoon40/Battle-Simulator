#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/resource.h>

#include "cli/cli_args.hpp"
#include "cli/replay.hpp"
#include "cli/replay_runtime.hpp"
#include "cli/report.hpp"
#include "cli/trace.hpp"
#include "sim/replay.hpp"
#include "sim/version.hpp"
#include "sim/events.hpp"
#include "cli/map_load.hpp"
#include "cli/mods.hpp"
#include "game/ascii.hpp"
#include "sim/terrain.hpp"
#include "game/camera.hpp"
#include "game/view.hpp"
#include <nlohmann/json.hpp>

#include "render/battle_view.hpp"
#include "render/png_writer.hpp"
#include "render/ui/demo.hpp"
#include "render/ui/screens.hpp"
#include "render/ui/ui.hpp"
#include "render/raster.hpp"
#include "sim/combat.hpp"
#include "sim/config.hpp"
#include "sim/factions.hpp"
#include "sim/json_export.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/unit_types.hpp"

namespace {

// Default data files, resolved relative to the working directory (the repo
// root when run as documented in the README).
constexpr const char* kDefaultConfigPath = "data/units.json";
constexpr const char* kDefaultScenarioPath = "data/scenarios/default.json";
constexpr const char* kDefaultPersonalitiesPath = "data/personalities.json";
constexpr const char* kDefaultMoralePath = "data/morale.json";
constexpr const char* kDefaultFormationsPath = "data/formations.json";
constexpr const char* kDefaultCommandersPath = "data/commanders.json";

// Peak resident set size of this process in MiB, via getrusage; used for the
// performance report ("memory consumption when measurable").
double peak_rss_mib() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return -1.0;
  }
  // ru_maxrss is in KiB on Linux.
  return static_cast<double>(usage.ru_maxrss) / 1024.0;
}

// Reads a whole file. All file I/O lives in the CLI; sim only parses text or
// streams. Missing and unreadable files produce clear errors.
std::string read_file(const std::string& path, const char* what) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    throw std::runtime_error(std::string(what) + " file '" + path +
                             "' does not exist");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error(std::string("cannot open ") + what + " file '" +
                             path + "'");
  }
  std::string text((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  if (input.bad()) {
    throw std::runtime_error(std::string("failed while reading ") + what +
                             " file '" + path + "'");
  }
  return text;
}

sim::UnitTypeTable load_unit_types(const std::string& path) {
  const std::string text = read_file(path, "unit type config");
  try {
    return sim::parse_unit_types(text);
  } catch (const std::invalid_argument& error) {
    throw std::runtime_error("invalid unit type config '" + path +
                             "': " + error.what());
  }
}

sim::Scenario load_scenario(const std::string& path) {
  const std::string text = read_file(path, "scenario");
  try {
    return sim::parse_scenario(text);
  } catch (const std::invalid_argument& error) {
    throw std::runtime_error("invalid scenario '" + path +
                             "': " + error.what());
  }
}

sim::PersonalityTable load_personalities(const std::string& path) {
  const std::string text = read_file(path, "personality table");
  try {
    return sim::parse_personalities(text);
  } catch (const std::invalid_argument& error) {
    throw std::runtime_error("invalid personality table '" + path +
                             "': " + error.what());
  }
}

sim::MoraleConfig load_morale(const std::string& path) {
  const std::string text = read_file(path, "morale config");
  try {
    return sim::parse_morale(text);
  } catch (const std::invalid_argument& error) {
    throw std::runtime_error("invalid morale config '" + path +
                             "': " + error.what());
  }
}

sim::CombatConfig load_combat(const std::string& path) {
  const std::string text = read_file(path, "combat config");
  try {
    return sim::parse_combat(text);
  } catch (const std::invalid_argument& error) {
    throw std::runtime_error("invalid combat config '" + path +
                             "': " + error.what());
  }
}

sim::TacticsConfig load_tactics(const std::string& path) {
  const std::string text = read_file(path, "tactics config");
  try {
    return sim::parse_tactics(text);
  } catch (const std::invalid_argument& error) {
    throw std::runtime_error("invalid tactics config '" + path +
                             "': " + error.what());
  }
}

sim::FormationConfig load_formations(const std::string& path) {
  const std::string text = read_file(path, "formation config");
  try {
    return sim::parse_formations(text);
  } catch (const std::invalid_argument& error) {
    throw std::runtime_error("invalid formation config '" + path +
                             "': " + error.what());
  }
}

sim::CommanderConfig load_commanders(const std::string& path) {
  const std::string text = read_file(path, "commander config");
  try {
    return sim::parse_commanders(text);
  } catch (const std::invalid_argument& error) {
    throw std::runtime_error("invalid commander config '" + path +
                             "': " + error.what());
  }
}

// m13 reliability review: strict unsigned parsing for the canonical
// "kill:UNIT:TICK" intervention strings (they are built by parse_args
// from validated numbers, but the re-parse must not rely on that
// invariant — strtoul would wrap "-1" and stop silently at "25junk").
namespace {

bool parse_u64_strict(const std::string& text, std::uint64_t& out) {
  if (text.empty() ||
      text.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  if (errno == ERANGE || end == nullptr || *end != '\0') {
    return false;
  }
  out = static_cast<std::uint64_t>(value);
  return true;
}

std::optional<cli::Intervention> parse_intervention(const std::string& spec) {
  if (spec.rfind("kill:", 0) != 0) {
    return std::nullopt;
  }
  const std::size_t first = spec.find(':', 5);
  if (first == std::string::npos) {
    return std::nullopt;
  }
  std::uint64_t unit = 0;
  std::uint64_t tick = 0;
  if (!parse_u64_strict(spec.substr(5, first - 5), unit) ||
      !parse_u64_strict(spec.substr(first + 1), tick) || unit == 0 ||
      tick == 0 || unit > 0xFFFFFFFFULL) {
    return std::nullopt;
  }
  cli::Intervention intervention;
  intervention.unit = static_cast<std::uint32_t>(unit);
  intervention.tick = tick;
  return intervention;
}

}  // namespace

int run_with_replay(const cli::Arguments& args,
                    cli::ReplayData replay_data) {
  // Whole-mod validation first (M8): an invalid mod is rejected with
  // readable errors before anything loads, so nothing is partial.
  if (!args.mod_dir.empty()) {
    const std::vector<std::string> problems =
        cli::mods::validate_mod(args.mod_dir);
    if (!problems.empty()) {
      for (const std::string& problem : problems) {
        std::cerr << "error: " << problem << "\n";
      }
      return 1;
    }
  }

  // Default data paths resolve through the mod overlay when one is active;
  // explicitly passed paths always win over both.
  const std::string config_path =
      !args.config_path.empty()
          ? args.config_path
          : cli::mods::data_file(args.mod_dir, "units.json").string();
  const std::string personalities_path =
      !args.personalities_path.empty()
          ? args.personalities_path
          : cli::mods::data_file(args.mod_dir, "personalities.json").string();
  const std::string morale_path =
      !args.morale_config_path.empty()
          ? args.morale_config_path
          : cli::mods::data_file(args.mod_dir, "morale.json").string();
  const std::string formations_path =
      !args.formations_path.empty()
          ? args.formations_path
          : cli::mods::data_file(args.mod_dir, "formations.json").string();
  const std::string commanders_path =
      !args.commanders_path.empty()
          ? args.commanders_path
          : cli::mods::data_file(args.mod_dir, "commanders.json").string();
  // The scenario may be a path or a bare name (resolved against the mod's
  // scenarios/ first, then data/scenarios/).
  std::string scenario_path;
  if (!args.scenario_path.empty()) {
    std::error_code ec;
    if (std::filesystem::exists(args.scenario_path, ec)) {
      scenario_path = args.scenario_path;
    } else {
      const std::filesystem::path resolved =
          cli::mods::resolve_scenario(args.mod_dir, args.scenario_path);
      if (resolved.empty()) {
        throw std::runtime_error(
            "scenario '" + args.scenario_path +
            "' is neither an existing file nor a scenario name (looked in " +
            (args.mod_dir.empty() ? std::string("data/scenarios/")
                                  : args.mod_dir + "/scenarios/ and data/"
                                                    "scenarios/") +
            ")");
      }
      scenario_path = resolved.string();
    }
  } else {
    scenario_path = kDefaultScenarioPath;
  }

  const sim::UnitTypeTable types = load_unit_types(config_path);
  const sim::Scenario scenario = load_scenario(scenario_path);
  // Terrain (M8): the type table loads when the scenario has a map (or a
  // mod replaces terrain.json); the map itself decodes here — sim/ only
  // ever sees a TerrainGrid.
  bool has_terrain_table = false;
  sim::TerrainTypeTable terrain_types;
  if (scenario.map.has_value() || cli::mods::has_file(args.mod_dir, "terrain.json")) {
    const std::string terrain_path =
        cli::mods::data_file(args.mod_dir, "terrain.json").string();
    try {
      terrain_types = sim::parse_terrain_types(read_file(terrain_path, "terrain config"));
      has_terrain_table = true;
    } catch (const std::invalid_argument& error) {
      throw std::runtime_error("invalid terrain config '" + terrain_path +
                               "': " + error.what());
    }
  }
  sim::TerrainGrid terrain_grid;
  if (scenario.map.has_value()) {
    if (!has_terrain_table) {
      throw std::runtime_error(
          "scenario '" + scenario_path + "' references map '" +
          scenario.map->file + "' but no terrain config exists (need " +
          cli::mods::data_file(args.mod_dir, "terrain.json").string() + ")");
    }
    const std::filesystem::path map_path =
        cli::mods::resolve_map(args.mod_dir, scenario.map->file);
    if (map_path.empty()) {
      throw std::runtime_error("map file '" + scenario.map->file +
                               "' not found (looked in data/maps/" +
                               (args.mod_dir.empty()
                                    ? std::string()
                                    : " and " + args.mod_dir + "/maps/") +
                               ")");
    }
    terrain_grid =
        cli::load_map(map_path.string(), scenario.map->cell_size, terrain_types);
  }
  const sim::PersonalityTable personalities =
      load_personalities(personalities_path);
  const sim::MoraleConfig morale = load_morale(morale_path);
  // Formation and commander configs load whenever squads can run (utility
  // mode, on): they are validated even for scenarios without squads, so a
  // corrupt file is reported instead of silently ignored.
  const sim::FormationConfig formations =
      args.utility_ai && args.squads_on
          ? load_formations(formations_path)
          : sim::FormationConfig{};
  const sim::CommanderConfig commanders =
      args.utility_ai && args.squads_on
          ? load_commanders(commanders_path)
          : sim::CommanderConfig{};
  // M10 tactics config: loaded whenever variance can run (utility mode,
  // on) so a corrupt file is reported instead of silently ignored.
  const std::string tactics_path =
      !args.tactics_path.empty()
          ? args.tactics_path
          : cli::mods::data_file(args.mod_dir, "tactics.json").string();
  const sim::TacticsConfig tactics =
      args.utility_ai && args.variance_on
          ? load_tactics(tactics_path)
          : sim::TacticsConfig{};
  // m13 item 3: combat depth config — loaded whenever utility mode can
  // run (validated even with --combat v1 so a corrupt file is reported
  // instead of silently ignored).
  const std::string combat_path =
      cli::mods::data_file(args.mod_dir, "combat.json").string();
  const sim::CombatConfig combat_config = args.utility_ai
                                               ? load_combat(combat_path)
                                               : sim::CombatConfig{};

  sim::SimulationConfig config;
  config.seed = args.seed;
  config.max_ticks = args.ticks;
  config.battlefield_width = scenario.battlefield_width;
  config.battlefield_height = scenario.battlefield_height;
  config.spawn_band_fraction = scenario.spawn_band_fraction;
  for (sim::Faction faction : sim::kAllFactions) {
    config.spawn_anchors[static_cast<std::size_t>(faction)] =
        scenario.anchors[static_cast<std::size_t>(faction)];
  }
  if (scenario.map.has_value()) {
    // The map defines the battlefield: map size x cell_size.
    config.battlefield_width = terrain_grid.width * terrain_grid.cell_size;
    config.battlefield_height =
        terrain_grid.height * terrain_grid.cell_size;
    config.terrain = &terrain_grid;
  }
  config.unit_types = &types;
  config.personalities = &personalities;
  config.morale_enabled = args.morale_on;
  config.morale = &morale;
  // M10: tactics on = combat variance + decision intervals + memory +
  // Search/Help/Flank (utility mode only; off reproduces M1-M9 exactly).
  config.tactics_enabled = args.variance_on && args.utility_ai;
  config.contact_seek_enabled = args.contact_seek_on && args.variance_on &&
                                args.utility_ai;
  config.tactics = &tactics;
  // m13 item 3: combat depth (utility mode only; the sim gates it).
  // M10 Part 2a: event recording + timeline sampling. Off by default;
  // --report-png needs the timeline, so it implies sampling. (render_mode
  // is computed here because the recorders must attach before the
  // simulation is constructed; the render branch below re-checks it.)
  const bool render_mode =
      args.render_frame > 0 || !args.render_frames.empty() ||
      args.ascii_tick > 0;
  std::ofstream events_file;
  sim::EventRecorder recorder;
  if (args.events_on && !render_mode) {
    events_file.open(args.events_path, std::ios::binary | std::ios::trunc);
    if (!events_file) {
      std::cerr << "error: cannot open events file '" << args.events_path
                << "' for writing\n";
      return 1;
    }
    recorder.attach_ostream(&events_file);
    config.events = &recorder;
  }
  if ((args.export_csv || args.report_png_on || args.report_on) &&
      !render_mode) {
    config.timeline_interval = args.csv_interval;
  }
  config.swap_sides = args.swap_sides;
  config.nav_v2 = args.nav_v2;
  config.ai_avail_v2 = args.ai_avail_v2;
  config.end_v2 = args.end_v2;
  config.combat_v2 = args.combat_v2;
  config.combat = &combat_config;
  config.orders_enabled = args.orders_on;

  // m11b: load and validate the order script (unless replaying — the
  // orders then come from the replay file). Scripts need orders on and
  // one of the loop-driven run paths.
  std::vector<cli::OrderIntervention> script_orders;
  if (!args.script_path.empty() && !args.replay_on) {
    if (!args.orders_on) {
      std::cerr << "error: --script needs --orders on\n";
      return 1;
    }
    if (render_mode || !args.tactic_scan.empty()) {
      std::cerr << "error: --script is not supported together with "
                   "--render-frame/--render-frames/--ascii or "
                   "--tactic-scan\n";
      return 1;
    }
    std::string script_error;
    if (!cli::parse_order_script(args.script_path, script_orders,
                                 script_error)) {
      std::cerr << "error: " << script_error << "\n";
      return 1;
    }
    for (const cli::OrderIntervention& order : script_orders) {
      if (order.type != sim::PlayerOrderType::ChangeFormation) {
        continue;
      }
      bool known = false;
      for (const sim::FormationConfig::Definition& definition :
           formations.formations) {
        if (definition.id == order.formation) {
          known = true;
          break;
        }
      }
      if (!known) {
        std::cerr << "error: order script: unknown formation '"
                  << order.formation << "' at tick " << order.tick << "\n";
        return 1;
      }
    }
  }
  for (sim::Faction faction : sim::kAllFactions) {
    config.spawn_patterns[static_cast<std::size_t>(faction)] =
        scenario.spawn_patterns[static_cast<std::size_t>(faction)];
  }

  // Optional per-faction objectives from the scenario; unset factions keep
  // the default (center of the opposing spawn band).
  for (sim::Faction faction : sim::kAllFactions) {
    const std::size_t index = static_cast<std::size_t>(faction);
    if (const auto& objective = scenario.objectives[index]) {
      config.objectives[index] =
          sim::ObjectivePoint{objective->x, objective->y, true};
    }
  }

  if (args.units_explicit) {
    // M1 mode: an infantry-only split of --units. Battlefield geometry and
    // infantry stats still come from the (default or explicit) data files;
    // with the shipped defaults both equal the M1 values, so `--units 500
    // --seed 42` reproduces M1 exactly.
    if (args.units > sim::kMaxUnits) {
      throw std::runtime_error(
          "--units must not exceed the " + std::to_string(sim::kMaxUnits) +
          "-unit cap (got " + std::to_string(args.units) + ")");
    }
    const int infantry = types.index_of("infantry");
    if (infantry < 0) {
      throw std::runtime_error("--units requires an 'infantry' unit type in '" +
                               config_path + "'");
    }
    const sim::ArmySizes sizes = sim::split_armies(args.units);
    config.composition[static_cast<std::size_t>(sim::Faction::Red)]
        .push_back(sim::TypeCount{static_cast<std::uint32_t>(infantry),
                                  sizes.red});
    config.composition[static_cast<std::size_t>(sim::Faction::Blue)]
        .push_back(sim::TypeCount{static_cast<std::uint32_t>(infantry),
                                  sizes.blue});
  } else {
    try {
      config.composition =
          sim::build_composition(scenario, types, args.seed);
    } catch (const std::invalid_argument& error) {
      throw std::runtime_error("invalid scenario '" + scenario_path +
                               "': " + error.what());
    }
  }

  std::uint32_t total_units = 0;
  for (const auto& army : config.composition) {
    for (const sim::TypeCount& entry : army) {
      total_units += entry.count;
    }
  }
  config.total_units = total_units;  // reporting/export only in this mode
  config.neighbor_mode =
      args.grid_mode ? sim::NeighborMode::Grid : sim::NeighborMode::Brute;
  config.grid_cell_size = args.grid_cell_size;
  config.ai_mode =
      args.utility_ai ? sim::AiMode::Utility : sim::AiMode::Legacy;
  // Squads (M6): plans only reach the simulation when squads are on, so
  // --squads off battles carry no squad state at all and reproduce M5
  // bit-for-bit. Legacy mode never carries plans.
  config.squads_enabled = args.squads_on && args.utility_ai;
  if (config.squads_enabled) {
    config.formations = &formations;
    config.commanders = &commanders;
    for (sim::Faction faction : sim::kAllFactions) {
      const std::size_t index = static_cast<std::size_t>(faction);
      if (const auto& plan = scenario.squads[index]) {
        config.squad_plans[index] =
            sim::SquadPlan{plan->count, plan->size};
      }
    }
  }

  // --swap-sides (M10): mirror the whole setup — compositions, explicit
  // objectives, squad plans and spawn patterns swap with the bands
  // (config.swap_sides flips which band each faction holds). Unit
  // factions themselves are untouched: Red simply deploys in the east.
  // m11a: the once-only objective swap (and its regression story) lives
  // in cli::apply_side_swap, unit-tested in test_cli_args.cpp; gated on
  // nav v2 so --nav v1 reproduces the exact M10 setup byte-for-byte.
  if (args.swap_sides) {
    if (args.nav_v2) {
      cli::apply_side_swap(config);
    } else {
      config.composition[static_cast<std::size_t>(sim::Faction::Red)]
          .swap(config.composition[static_cast<std::size_t>(sim::Faction::Blue)]);
      config.squad_plans[static_cast<std::size_t>(sim::Faction::Red)]
          .swap(config.squad_plans[static_cast<std::size_t>(sim::Faction::Blue)]);
      for (sim::Faction faction : sim::kAllFactions) {
        const std::size_t index = static_cast<std::size_t>(faction);
        const std::size_t other = 1 - index;
        std::swap(config.spawn_patterns[index], config.spawn_patterns[other]);
        std::swap(config.spawn_anchors[index], config.spawn_anchors[other]);
      }
    }
  }

  // m13 item 4: resolve --threads. 0 = one worker per hardware core
  // (at least 1; hardware_concurrency is only a hint and may return 0).
  // 1 (default) runs every phase inline — byte-identical to the
  // pre-threads binary. Output is identical for every thread count.
  std::uint32_t thread_count = args.threads;
  if (thread_count == 0) {
    const unsigned hardware = std::thread::hardware_concurrency();
    thread_count = hardware == 0 ? 1u : static_cast<std::uint32_t>(hardware);
    std::cout << "threads:              " << thread_count
              << " (hardware concurrency)\n";
  }
  config.thread_count = thread_count;

  sim::Simulation simulation(config);
  // m13 item 4: --profile-phases — clock reads only; the battle is
  // byte-identical with profiling on or off.
  simulation.enable_phase_profiling(args.profile_phases);
  // m11b: applies one scripted order (also used by the record/replay
  // loops). "all" addresses every squad of the faction at issue time.
  const auto apply_script_order =
      [&simulation](const cli::OrderIntervention& order) {
        const sim::Faction faction =
            order.red ? sim::Faction::Red : sim::Faction::Blue;
        std::vector<std::uint32_t> squads;
        if (order.squads_all) {
          for (const sim::Squad& squad : simulation.squads()) {
            if (squad.faction == faction) {
              squads.push_back(squad.id);
            }
          }
        } else {
          squads = order.squads;
        }
        if (!simulation.issue_order(faction, squads, order.type, order.x,
                                    order.y, order.formation)) {
          std::cerr << "warning: order at tick " << order.tick << " ("
                    << sim::to_string(order.type)
                    << ") was rejected (invalid squad, rate limit or "
                       "disabled orders)\n";
        }
      };
  if (args.ai_debug_unit != 0 || !args.tactic_scan.empty()) {
    sim::AiDebugCapture debug;
    debug.unit_id = args.ai_debug_unit;
    debug.tick = args.ai_debug_tick;
    debug.explicit_tick = args.ai_debug_tick_explicit;
    simulation.set_ai_debug(debug);
  }

  // Headless rendering / ascii map mode (M7): run tick-by-tick to each
  // requested target, render or print on arrival, and stop after the last
  // one (the battle is not simulated further; the report below reflects
  // the state at the last target).
  const auto start = std::chrono::steady_clock::now();
  sim::BattleResult result;
  if (!args.ui_script.empty()) {
    // m11c: the headless UI harness — drive the demo control panel
    // with a scripted event sequence and write one PNG per frame plus a
    // per-frame state line (tests pin both).
    std::ifstream script_file(args.ui_script);
    if (!script_file) {
      std::cerr << "error: cannot open UI script '" << args.ui_script
                << "'\n";
      return 1;
    }
    nlohmann::json script;
    try {
      script_file >> script;
    } catch (const nlohmann::json::exception& error) {
      std::cerr << "error: UI script is not valid JSON: " << error.what()
                << "\n";
      return 1;
    }
    const auto script_uint = [&](const char* key,
                                 std::uint64_t fallback) {
      return script.contains(key) && script[key].is_number_unsigned()
                 ? script[key].get<std::uint64_t>()
                 : fallback;
    };
    if (!script.contains("width") || !script.contains("height") ||
        !script.contains("out") || !script.contains("events")) {
      std::cerr << "error: UI script needs width, height, out and "
                   "events\n";
      return 1;
    }
    // m13 reliability review: every integer field is read through a
    // range-checked integer reader. nlohmann's get<int> is an unchecked
    // static_cast — a JSON double like 1e300 (or a 64-bit integer
    // outside int range) was undefined behavior; non-integers now fall
    // back (optional fields) or produce a clear error (required ones).
    const auto json_int = [](const nlohmann::json& node, const char* key,
                             long long& out) {
      if (!node.contains(key) || !node[key].is_number_integer()) {
        return false;
      }
      const long long value = node[key].get<long long>();
      if (value < -1000000000LL || value > 1000000000LL) {
        return false;  // outside any sane UI number; never converts badly
      }
      out = value;
      return true;
    };
    const auto script_int = [&](const char* key, long long& out) {
      return json_int(script, key, out);
    };
    long long ui_width_ll = 0;
    long long ui_height_ll = 0;
    if (!script_int("width", ui_width_ll) ||
        !script_int("height", ui_height_ll)) {
      std::cerr << "error: UI script width and height must be whole "
                   "numbers\n";
      return 1;
    }
    if (ui_width_ll < 200 || ui_height_ll < 150 || ui_width_ll > 4096 ||
        ui_height_ll > 4096) {
      std::cerr << "error: UI script size must be in [200x150, "
                   "4096x4096]\n";
      return 1;
    }
    const int ui_width = static_cast<int>(ui_width_ll);
    const int ui_height = static_cast<int>(ui_height_ll);
    const std::uint64_t ui_tick = script_uint("tick", 0);
    const std::string out_pattern = script["out"].get<std::string>();

    // The scripted events, grouped by frame.
    struct ScriptedEvent {
      render::ui::Event event;
      int frame = 0;
    };
    const auto parse_key = [](const std::string& name,
                              render::ui::Key& out) {
      if (name == "tab") out = render::ui::Key::Tab;
      else if (name == "enter") out = render::ui::Key::Enter;
      else if (name == "escape") out = render::ui::Key::Escape;
      else if (name == "up") out = render::ui::Key::Up;
      else if (name == "down") out = render::ui::Key::Down;
      else if (name == "left") out = render::ui::Key::Left;
      else if (name == "right") out = render::ui::Key::Right;
      else if (name == "home") out = render::ui::Key::Home;
      else if (name == "end") out = render::ui::Key::End;
      else return false;
      return true;
    };
    std::vector<ScriptedEvent> scripted;
    int frame_count = 1;
    for (const nlohmann::json& entry : script["events"]) {
      if (!entry.is_object() || !entry.contains("frame") ||
          !entry.contains("type")) {
        std::cerr << "error: every UI script event needs frame and "
                     "type\n";
        return 1;
      }
      ScriptedEvent item;
      long long frame = -1;
      if (!json_int(entry, "frame", frame) || frame < 0) {
        std::cerr << "error: every UI script event needs a non-negative "
                     "whole-number frame\n";
        return 1;
      }
      item.frame = static_cast<int>(frame);
      frame_count = std::max(frame_count, item.frame + 1);
      const std::string type = entry["type"].get<std::string>();
      // Optional integer fields keep their fallback for missing,
      // non-integer or out-of-range values (no unchecked conversions).
      const auto number = [&](const char* key, int fallback) {
        long long value = 0;
        return json_int(entry, key, value)
                   ? static_cast<int>(value)
                   : fallback;
      };
      if (type == "key") {
        if (!entry.contains("key") ||
            !parse_key(entry["key"].get<std::string>(), item.event.key)) {
          std::cerr << "error: UI script key event needs a key (tab, "
                       "enter, escape, up, down, left, right, home, "
                       "end)\n";
          return 1;
        }
        item.event.type = render::ui::Event::Type::Key;
      } else if (type == "move" || type == "down" || type == "up") {
        item.event.type =
            type == "move" ? render::ui::Event::Type::MouseMove
            : type == "down" ? render::ui::Event::Type::MouseDown
                             : render::ui::Event::Type::MouseUp;
        item.event.pos = render::ui::Point{number("x", 0),
                                           number("y", 0)};
        item.event.button = number("button", 0);
      } else if (type == "wheel") {
        item.event.type = render::ui::Event::Type::Wheel;
        item.event.delta = number("delta", 0);
        item.event.pos = render::ui::Point{number("x", 0),
                                           number("y", 0)};
      } else {
        std::cerr << "error: unknown UI script event type '" << type
                  << "'\n";
        return 1;
      }
      scripted.push_back(item);
    }

    // The battle backdrop: the simulation advanced to `tick`, rendered
    // once (the battle is frozen while the UI is driven).
    while (simulation.tick_index() < ui_tick && !simulation.finished()) {
      simulation.tick();
    }
    const game::Camera camera = game::Camera::fit(
        simulation.world().width, simulation.world().height, ui_width,
        ui_height);
    const render::ui::Theme ui_theme = render::ui::default_theme();
    // m11d: the script picks the starting screen (menu / scenario /
    // battle, default battle); the scenario list comes from the script
    // or the shipped data/scenarios directory (sorted, deterministic).
    std::vector<std::string> scenario_names;
    if (script.contains("scenarios") && script["scenarios"].is_array()) {
      for (const nlohmann::json& name : script["scenarios"]) {
        if (name.is_string()) {
          scenario_names.push_back(name.get<std::string>());
        }
      }
    } else {
      for (const auto& entry :
           std::filesystem::directory_iterator("data/scenarios")) {
        if (entry.path().extension() == ".json") {
          scenario_names.push_back(entry.path().stem().string());
        }
      }
      std::sort(scenario_names.begin(), scenario_names.end());
    }
    const std::string start_screen =
        script.contains("screen") && script["screen"].is_string()
            ? script["screen"].get<std::string>()
            : "battle";
    if (start_screen != "menu" && start_screen != "scenario" &&
        start_screen != "battle") {
      std::cerr << "error: UI script screen must be menu, scenario or "
                   "battle\n";
      return 1;
    }
    render::ui::App app =
        render::ui::sandbox_app(ui_width, ui_height, scenario_names);
    if (start_screen == "menu") {
      app.screen = render::ui::Screen::Menu;
    } else if (start_screen == "scenario") {
      app.screen = render::ui::Screen::Scenario;
    } else {
      app.screen = render::ui::Screen::Battle;
    }
    for (int frame = 0; frame < frame_count; ++frame) {
      for (const ScriptedEvent& item : scripted) {
        if (item.frame != frame) {
          continue;
        }
        app = render::ui::reduce_app(std::move(app), item.event,
                                     scenario_names);
      }
      render::DrawList list = render::build_battle_draw_list(
          simulation, camera,
          render::ui::options_from(app.battle));
      list.width = ui_width;
      list.height = ui_height;
      const render::ui::State& screen_state = app.active();
      if (app.screen != render::ui::Screen::Battle) {
        // The menu and scenario screens draw over a plain background.
        list.background = game::background_color();
        list.cmds.clear();
      }
      render::ui::draw_ui(screen_state, ui_theme, list);
      const render::Image image = render::rasterize(list);
      std::string path = out_pattern;
      const std::string marker = "%d";
      const std::size_t at = path.find(marker);
      if (at != std::string::npos) {
        path.replace(at, marker.size(), std::to_string(frame));
      }
      const std::filesystem::path file_path(path);
      if (file_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(file_path.parent_path(), ec);
      }
      if (!render::write_png(image, path)) {
        std::cerr << "error: cannot write PNG '" << path << "'\n";
        return 1;
      }
      // The state line: screen, focus, hover, the last click, plus the
      // battle values (tests pin these).
      const auto screen_name = [&] {
        switch (app.screen) {
          case render::ui::Screen::Menu:
            return "menu";
          case render::ui::Screen::Scenario:
            return "scenario";
          case render::ui::Screen::Battle:
            break;
        }
        return "battle";
      }();
      std::cout << "ui frame " << frame << " screen " << screen_name
                << " focus "
                << (screen_state.focus >= 0
                        ? screen_state.tree.at(static_cast<std::size_t>(
                              screen_state.focus))
                              .id
                        : "-")
                << " hover "
                << (screen_state.hover >= 0
                        ? screen_state.tree.at(static_cast<std::size_t>(
                              screen_state.hover))
                              .id
                        : "-")
                << " quit " << (app.quit ? 1 : 0);
      if (app.screen == render::ui::Screen::Battle) {
        const auto widget_by_id = [&](const std::string& id) {
          for (std::size_t i = 0; i < app.battle.tree.size(); ++i) {
            if (app.battle.tree.at(i).id == id) {
              return static_cast<std::int32_t>(i);
            }
          }
          return -1;
        };
        const auto& zoom = app.battle.tree.at(static_cast<std::size_t>(
            std::max<std::int32_t>(0, widget_by_id("zoom"))));
        const auto& filter = app.battle.tree.at(
            static_cast<std::size_t>(
                std::max<std::int32_t>(0, widget_by_id("filter"))));
        std::cout << " zoom " << zoom.value << " filter "
                  << filter.selected;
      }
      std::cout << " -> " << path << "\n";
    }
    result = simulation.result();
  } else if (render_mode) {
    std::vector<std::uint64_t> render_targets;
    if (args.render_frame > 0) {
      render_targets.push_back(args.render_frame);
    }
    if (!args.render_frames.empty()) {
      std::size_t pos = 0;
      while (pos <= args.render_frames.size()) {
        const std::size_t comma = args.render_frames.find(',', pos);
        const std::string item =
            args.render_frames.substr(pos, comma == std::string::npos
                                               ? std::string::npos
                                               : comma - pos);
        if (item.empty() ||
            item.find_first_not_of("0123456789") != std::string::npos) {
          std::cerr << "error: --render-frames expects comma-separated tick "
                       "numbers, got '"
                    << item << "'\n";
          return 1;
        }
        // Digits-only, but a 20+ digit tick overflows uint64: stoull
        // throws out_of_range, which used to escape and abort the CLI.
        try {
          render_targets.push_back(std::stoull(item));
        } catch (const std::exception&) {
          std::cerr << "error: --render-frames tick '" << item
                    << "' is out of range\n";
          return 1;
        }
        if (comma == std::string::npos) {
          break;
        }
        pos = comma + 1;
      }
    }
    std::sort(render_targets.begin(), render_targets.end());
    render_targets.erase(std::unique(render_targets.begin(),
                                     render_targets.end()),
                         render_targets.end());
    std::vector<std::uint64_t> stops = render_targets;
    if (args.ascii_tick > 0) {
      stops.push_back(args.ascii_tick);
    }
    std::sort(stops.begin(), stops.end());
    stops.erase(std::unique(stops.begin(), stops.end()), stops.end());

    const game::Camera camera = game::Camera::fit(
        simulation.world().width, simulation.world().height,
        render::kFrameWidth, render::kFrameHeight);
    game::ViewOptions options;  // the default demo view

    for (const std::uint64_t target : stops) {
      while (simulation.tick_index() < target && !simulation.finished()) {
        simulation.tick();
      }
      if (args.ascii_tick > 0 && target == args.ascii_tick) {
        std::cout << game::ascii_map(simulation) << "\n";
      }
      if (std::binary_search(render_targets.begin(), render_targets.end(),
                             target)) {
        const auto png_start = std::chrono::steady_clock::now();
        const render::DrawList list = render::build_battle_draw_list(
            simulation, camera, options);
        const render::Image image = render::rasterize(list);
        std::string path;
        if (args.render_frame == target && !args.render_out.empty()) {
          path = args.render_out;
        } else {
          path = args.render_out_prefix + "-" +
                 std::to_string(target / 1000000 % 10) +
                 std::to_string(target / 100000 % 10) +
                 std::to_string(target / 10000 % 10) +
                 std::to_string(target / 1000 % 10) +
                 std::to_string(target / 100 % 10) +
                 std::to_string(target / 10 % 10) +
                 std::to_string(target % 10) + ".png";
        }
        const std::filesystem::path file_path(path);
        if (file_path.has_parent_path()) {
          std::error_code ec;
          std::filesystem::create_directories(file_path.parent_path(), ec);
          if (ec) {
            std::cerr << "error: cannot create render directory '"
                      << file_path.parent_path().string() << "': "
                      << ec.message() << "\n";
            return 1;
          }
        }
        if (!render::write_png(image, path)) {
          std::cerr << "error: cannot write PNG '" << path << "'\n";
          return 1;
        }
        const auto png_end = std::chrono::steady_clock::now();
        const double png_ms =
            std::chrono::duration<double>(png_end - png_start).count() *
            1000.0;
        std::cout << "PNG render: tick " << simulation.tick_index()
                  << " -> " << path << " (" << std::fixed
                  << std::setprecision(1) << png_ms << " ms, "
                  << image.width << "x" << image.height << ")\n";
      }
    }
    result = simulation.result();
  } else if (!args.tactic_scan.empty()) {
    // --tactic-scan (M10): tick the battle manually and arm the AI debug
    // capture on the first unit that chooses the requested action, so the
    // end-of-run report shows a real, on-the-spot decision block.
    const sim::Action wanted = [](const std::string& name) {
      if (name == "search") return sim::Action::Search;
      if (name == "help") return sim::Action::Help;
      return sim::Action::Flank;
    }(args.tactic_scan);
    // Arm the capture on the first unit seen choosing the action, then
    // keep going: the capture records that unit's NEXT decision (a tick
    // later), so if the unit switched actions, re-arm on another unit
    // until some captured decision really is the wanted action (bounded
    // re-arms so a rare action still terminates).
    int re_arms = 0;
    bool captured_wanted = false;
    while (!simulation.finished() && !captured_wanted) {
      simulation.tick();
      const bool have_capture =
          simulation.ai_debug().captured &&
          simulation.ai_debug().chosen == wanted;
      if (have_capture) {
        captured_wanted = true;
        break;
      }
      if (simulation.ai_debug().captured) {
        // The armed unit decided something else; forget it and rescan.
        sim::AiDebugCapture clear;
        simulation.set_ai_debug(clear);
      }
      if (re_arms < 32) {
        for (const sim::Unit& unit : simulation.world().units) {
          if (unit.alive &&
              static_cast<sim::Action>(unit.last_action) == wanted) {
            sim::AiDebugCapture capture;
            capture.unit_id = unit.id;
            capture.tick = simulation.tick_index() + 1;
            capture.explicit_tick = true;
            simulation.set_ai_debug(capture);
            ++re_arms;
            break;
          }
        }
      }
    }
    result = simulation.result();
    if (!captured_wanted) {
      std::cout << "Tactic scan: no unit ever chose '" << args.tactic_scan
                << "' in this battle.\n";
    }
  } else if (args.record_on || args.replay_on) {
    // M10 Part 2b: tick-by-tick so interventions can land between ticks
    // and state hashes can be taken every 25 ticks (plus the final one).
    std::vector<cli::Intervention> interventions;
    if (args.replay_on) {
      interventions = replay_data.interventions;
    } else {
      for (const std::string& spec : args.interventions) {
        const std::optional<cli::Intervention> intervention =
            parse_intervention(spec);
        if (!intervention) {
          std::cerr << "error: invalid --intervene spec '" << spec << "'\n";
          return 1;
        }
        interventions.push_back(*intervention);
      }
      std::sort(interventions.begin(), interventions.end(),
                [](const cli::Intervention& a, const cli::Intervention& b) {
                  return a.tick < b.tick ||
                         (a.tick == b.tick && a.unit < b.unit);
                });
    }
    // m11b: replaying applies the recorded orders; recording applies the
    // --script orders (both ascending by tick). m13 item 5b: the loop is
    // the shared cli::ReplayDriver (the window's replay viewer runs the
    // exact same semantics); warnings surface exactly as before.
    const std::vector<cli::OrderIntervention>& order_script =
        args.replay_on ? replay_data.order_interventions : script_orders;
    std::vector<std::string> driver_warnings;
    cli::ReplayDriver driver(simulation, interventions, order_script);
    const cli::ReplayDriver::RunResult run = driver.run(
        args.replay_seek, args.record_on ? &replay_data : nullptr,
        args.replay_on ? &replay_data : nullptr, &driver_warnings);
    for (const std::string& warning : driver_warnings) {
      std::cerr << warning << "\n";
    }
    const bool mismatch = run.mismatch;
    if (mismatch) {
      std::cout << "replay verification FAILED\n"
                << "  first mismatching tick: " << run.mismatch_tick << "\n"
                << "  expected " << run.expected << "\n"
                << "  got      " << run.got << "\n";
    }
    if (args.record_on) {
      replay_data.version = sim::kSimVersion;
      // Record the resolved scenario path (explicit flag, bare name or
      // the default), never the raw flag value — `--record` without
      // `--scenario` used to write an empty scenario line, and every
      // such recording then failed --verify with "no scenario line".
      replay_data.scenario = scenario_path;
      replay_data.mod = args.mod_dir.empty() ? "-" : args.mod_dir;
      replay_data.seed = args.seed;
      replay_data.ticks = args.ticks;
      replay_data.variance = args.variance_on ? "on" : "off";
      replay_data.contact_seek = args.contact_seek_on ? "on" : "off";
      replay_data.neighbor = args.grid_mode ? "grid" : "brute";
      replay_data.squads = args.squads_on ? "on" : "off";
      replay_data.swap_sides = args.swap_sides ? "on" : "off";
      replay_data.nav = args.nav_v2 ? "v2" : "v1";
      replay_data.ai_avail = args.ai_avail_v2 ? "v2" : "v1";
      replay_data.end = args.end_v2 ? "v2" : "v1";
      replay_data.combat = args.combat_v2 ? "v2" : "v1";
      replay_data.orders = args.orders_on ? "on" : "off";
      replay_data.order_interventions = script_orders;
      for (const std::string& spec : args.interventions) {
        const std::optional<cli::Intervention> intervention =
            parse_intervention(spec);
        if (!intervention) {
          std::cerr << "error: invalid --intervene spec '" << spec << "'\n";
          return 1;
        }
        replay_data.interventions.push_back(*intervention);
      }
      if (!cli::write_replay(args.record_path, replay_data)) {
        std::cerr << "error: cannot write replay file '" << args.record_path
                  << "'\n";
        return 1;
      }
      std::cout << "replay recorded to '" << args.record_path << "' ("
                << replay_data.hashes.size() << " checkpoints)\n";
    } else if (mismatch) {
      return 2;
    } else if (args.replay_verify) {
      if (run.checkpoints_verified < replay_data.hashes.size()) {
        std::cout << "replay verification FAILED\n  replay ended at tick "
                  << simulation.world().tick_count << " with "
                  << replay_data.hashes.size() - run.checkpoints_verified
                  << " unverified checkpoints\n";
        return 2;
      }
      std::cout << "replay verified: " << replay_data.hashes.size()
                << " checkpoints match over " << simulation.world().tick_count
                << " ticks\n";
    }
    result = simulation.result();
  } else if (args.trace_on || !script_orders.empty()) {
    // m11a/m11b: --trace drives the loop here so a line can be written
    // every --trace-interval ticks (and once at the end); --script
    // (without record/replay) drives it to apply timed orders between
    // ticks. The tick sequence is exactly simulation.run()'s, so a
    // traced battle is bit-identical.
    std::ofstream trace_file;
    if (args.trace_on) {
      trace_file.open(args.trace_path);
      if (!trace_file) {
        std::cerr << "error: cannot open trace file '" << args.trace_path
                  << "'\n";
        return 1;
      }
    }
    std::size_t next_order = 0;
    while (!simulation.finished()) {
      const std::uint64_t upcoming = simulation.world().tick_count + 1;
      while (next_order < script_orders.size() &&
             script_orders[next_order].tick <= upcoming) {
        apply_script_order(script_orders[next_order]);
        ++next_order;
      }
      simulation.tick();
      if (args.trace_on &&
          (simulation.world().tick_count % args.trace_interval == 0 ||
           simulation.finished())) {
        trace_file << cli::trace_line(simulation);
      }
    }
    // m13 audit finding 12: a trace line that failed to write (full
    // disk) must fail the run — the old code exited 0 with a truncated
    // trace.
    if (args.trace_on) {
      trace_file.flush();
      if (!trace_file) {
        std::cerr << "error: failed while writing trace file '"
                  << args.trace_path << "'\n";
        return 1;
      }
    }
    result = simulation.result();
  } else {
    result = simulation.run();
  }
  const auto end = std::chrono::steady_clock::now();

  const double seconds =
      std::chrono::duration<double>(end - start).count();
  const std::uint64_t ticks = result.ticks_elapsed;
  const double ticks_per_second =
      seconds > 0.0 ? static_cast<double>(ticks) / seconds : 0.0;
  const double ms_per_tick =
      ticks > 0 ? seconds * 1000.0 / static_cast<double>(ticks) : 0.0;

  std::string export_report = "none";
  if (args.export_json) {
    const std::string json = sim::export_state_json(simulation);
    const std::filesystem::path path(args.export_path);
    if (path.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      if (ec) {
        std::cerr << "error: cannot create export directory '"
                  << path.parent_path().string() << "': " << ec.message()
                  << "\n";
        return 1;
      }
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
      std::cerr << "error: cannot open JSON export file '"
                << args.export_path << "' for writing\n";
      return 1;
    }
    file << json;
    file << '\n';
    file.flush();
    if (!file) {
      std::cerr << "error: failed while writing JSON export file '"
                << args.export_path << "'\n";
      return 1;
    }
    export_report = args.export_path;
  }

  // m13 audit finding 12: the event recorder wrote through events_file
  // during the run — a write failure (full disk, quota) must fail the
  // run with a nonzero exit instead of silently producing a truncated
  // events file.
  if (args.events_on && !render_mode) {
    events_file.flush();
    if (!events_file) {
      std::cerr << "error: failed while writing events file '"
                << args.events_path << "'\n";
      return 1;
    }
  }

  // M10 Part 2a deliverables (full-run mode only).
  if (args.export_csv && !render_mode) {
    std::ofstream csv_file(args.csv_path, std::ios::binary |
                                              std::ios::trunc);
    if (!csv_file) {
      std::cerr << "error: cannot open CSV timeline file '"
                << args.csv_path << "' for writing\n";
      return 1;
    }
    csv_file << cli::generate_timeline_csv(simulation, types);
    csv_file.flush();
    if (!csv_file) {
      std::cerr << "error: failed while writing CSV timeline file '"
                << args.csv_path << "'\n";
      return 1;
    }
  }
  if (args.report_on && !render_mode) {
    const cli::ReportMeta meta{args.scenario_path, args.seed};
    std::ofstream md_file(args.report_path, std::ios::binary |
                                                std::ios::trunc);
    if (!md_file) {
      std::cerr << "error: cannot open report file '" << args.report_path
                << "' for writing\n";
      return 1;
    }
    md_file << cli::generate_report_markdown(simulation, &recorder, types,
                                             meta);
    md_file.flush();
    if (!md_file) {
      std::cerr << "error: failed while writing report file '"
                << args.report_path << "'\n";
      return 1;
    }
    std::filesystem::path json_path(args.report_path);
    json_path.replace_extension(".json");
    std::ofstream json_file(json_path, std::ios::binary |
                                           std::ios::trunc);
    if (json_file) {
      json_file << cli::generate_report_json(simulation, &recorder, types,
                                             meta);
      json_file.flush();
      if (!json_file) {
        std::cerr << "error: failed while writing report JSON file '"
                  << json_path.string() << "'\n";
        return 1;
      }
    } else {
      std::cerr << "error: cannot open report JSON file '"
                << json_path.string() << "' for writing\n";
      return 1;
    }
  }
  if (args.report_png_on && !render_mode) {
    if (!cli::write_report_png(args.report_png_path, simulation)) {
      std::cerr << "error: cannot write report PNG '"
                << args.report_png_path << "'\n";
      return 1;
    }
  }

  const sim::FactionReport& red =
      result.factions[static_cast<std::size_t>(sim::Faction::Red)];
  const sim::FactionReport& blue =
      result.factions[static_cast<std::size_t>(sim::Faction::Blue)];

  std::ostringstream report;
  report.imbue(std::locale::classic());
  report << std::fixed << std::setprecision(6);

  report << "=== Battle Report ===\n"
         << "Seed:                " << args.seed << "\n"
         << "Units:               " << total_units << " (Red " << red.initial
         << " / Blue " << blue.initial << ")\n"
         << "Ticks elapsed:       " << ticks << " / " << args.ticks << "\n"
         << "Termination reason:  " << sim::to_string(result.reason) << "\n"
         << "Winner:              "
         << (result.has_winner ? sim::to_string(result.winner) : "draw")
         << "\n"
         << "Red survivors:       " << red.survivors << "\n"
         << "Red deaths:          " << red.deaths << "\n"
         << "Blue survivors:      " << blue.survivors << "\n"
         << "Blue deaths:         " << blue.deaths << "\n"
         << "Total deaths:        " << (red.deaths + blue.deaths) << "\n"
         << "Unit types:          " << config_path << "\n"
         << "Scenario:            " << scenario_path << "\n"
         << "Neighbor mode:       " << sim::to_string(config.neighbor_mode)
         << "\n"
         << "AI mode:             " << sim::to_string(config.ai_mode) << "\n";
  if (config.neighbor_mode == sim::NeighborMode::Grid) {
    report << "Grid cell size:      " << config.grid_cell_size << "\n";
  }
  if (config.ai_mode == sim::AiMode::Utility) {
    report << "Personalities:       " << personalities_path << "\n";
    report << "Morale:              "
           << (config.morale_enabled ? "on" : "off") << " (" << morale_path
           << ")\n";
    report << "Squads:              "
           << (config.squads_enabled ? "on" : "off");
    if (config.squads_enabled) {
      const auto& red_plan =
          config.squad_plans[static_cast<std::size_t>(sim::Faction::Red)];
      const auto& blue_plan =
          config.squad_plans[static_cast<std::size_t>(sim::Faction::Blue)];
      if (red_plan || blue_plan) {
        report << " (Red "
               << (red_plan ? std::to_string(red_plan->count) + " x " +
                                  std::to_string(red_plan->size)
                            : std::string("none"))
               << " / Blue "
               << (blue_plan ? std::to_string(blue_plan->count) + " x " +
                                  std::to_string(blue_plan->size)
                            : std::string("none"))
               << ")";
      } else {
        report << " (no squads in scenario)";
      }
    }
    report << "\n";
    if (config.squads_enabled) {
      report << "Formations:          " << formations_path << "\n";
      report << "Commanders:          " << commanders_path << "\n";
    }
  }
  if (config.ai_mode == sim::AiMode::Utility && config.morale_enabled) {
    report << "Red active:          " << red.active << " (routed "
           << (red.survivors - red.active) << ")\n"
           << "Blue active:         " << blue.active << " (routed "
           << (blue.survivors - blue.active) << ")\n";
    const sim::FactionReport* factions[] = {&red, &blue};
    report << "Routing stats:\n";
    for (const sim::FactionReport* faction : factions) {
      report << "  " << std::left << std::setw(5)
             << sim::to_string(faction == &red ? sim::Faction::Red
                                               : sim::Faction::Blue)
             << std::right << ": first rout tick "
             << (faction->routing.first_rout_tick == 0
                     ? std::string("never")
                     : std::to_string(faction->routing.first_rout_tick))
             << ", peak routed " << faction->routing.peak_routed
             << ", total routs " << faction->routing.total_routs
             << ", recoveries " << faction->routing.recoveries << "\n";
    }
  }

  if (simulation.squads_active()) {
    const sim::FactionReport* factions[] = {&red, &blue};
    report << "Squad stats:\n";
    for (const sim::FactionReport* faction : factions) {
      const sim::SquadStats& squad = faction->squad;
      report << "  " << std::left << std::setw(5)
             << sim::to_string(faction == &red ? sim::Faction::Red
                                               : sim::Faction::Blue)
             << std::right << ": squads " << squad.squads
             << ", commander deaths " << squad.commander_deaths
             << ", successions " << squad.successions
             << ", drops from formation " << squad.drops_from_formation
             << "\n      orders: advance "
             << squad.order_counts[static_cast<std::size_t>(sim::Order::Advance)]
             << ", hold "
             << squad.order_counts[static_cast<std::size_t>(sim::Order::Hold)]
             << ", regroup "
             << squad.order_counts[static_cast<std::size_t>(sim::Order::Regroup)]
             << ", retreat "
             << squad.order_counts[static_cast<std::size_t>(sim::Order::Retreat)]
             << "\n";
    }
  }

  if (config.ai_mode == sim::AiMode::Utility) {
    const sim::FactionReport* factions[] = {&red, &blue};
    report << "Action totals (cumulative):\n";
    for (const sim::FactionReport* faction : factions) {
      report << "  " << std::left << std::setw(5)
             << sim::to_string(faction == &red ? sim::Faction::Red
                                               : sim::Faction::Blue)
             << std::right << ": attack " << faction->action_counts[static_cast<std::size_t>(sim::Action::Attack)]
             << ", approach "
             << faction->action_counts[static_cast<std::size_t>(sim::Action::Approach)]
             << ", advance "
             << faction->action_counts[static_cast<std::size_t>(sim::Action::Advance)]
             << ", retreat "
             << faction->action_counts[static_cast<std::size_t>(sim::Action::Retreat)]
             << ", hold "
             << faction->action_counts[static_cast<std::size_t>(sim::Action::Hold)]
             << "\n";
    }
  }

  // AI debug output (utility mode): the capture happened during run(); print
  // what the unit saw, all raw action scores and the chosen action.
  if (args.ai_debug_unit != 0 || !args.tactic_scan.empty()) {
    const sim::AiDebugCapture& debug = simulation.ai_debug();
    report << "\nAI debug (unit "
            << (args.ai_debug_unit != 0 ? args.ai_debug_unit
                                        : simulation.ai_debug().unit_id)
            << "):\n";
    if (!debug.unit_found) {
      report << "  no such unit id in this battle\n";
    } else if (!debug.captured) {
      report << "  no capture: the unit never saw an enemy before it died or "
                "the battle ended (use --ai-debug-tick to pick a tick)\n";
    } else {
      const sim::PersonalityTable& table = simulation.personality_table();
      const char* archetype =
          debug.personality.archetype != sim::kCustomArchetype
              ? table.archetypes[debug.personality.archetype].id.c_str()
              : "custom";
      report << "  tick:                " << debug.captured_tick << "\n"
             << "  health:              " << debug.health << " / "
             << debug.max_health << "\n"
             << "  morale:              " << debug.morale << " / 100 ("
             << sim::to_string(
                    static_cast<sim::MoraleState>(debug.morale_state))
             << (debug.routed ? ", routed" : "") << ")\n"
             << "  personality:         " << archetype
             << " (bravery " << debug.personality.bravery
             << ", caution " << debug.personality.caution
             << ", aggression " << debug.personality.aggression << ")\n"
             << "  visible enemies:     " << debug.visible_enemies << "\n"
             << "  visible allies:      " << debug.visible_allies << "\n";
      if (debug.target >= 0) {
        report << "  nearest enemy:       index " << debug.target
               << " at distance " << debug.target_distance << "\n";
      } else {
        report << "  nearest enemy:       none\n";
      }
      for (std::size_t action = 0; action < sim::kActionCount; ++action) {
        const auto the_action = static_cast<sim::Action>(action);
        report << "  score " << std::left << std::setw(9)
               << sim::to_string(the_action) << std::right
               << debug.scores[action] << "\n";
      }
      // The order term (M6): what the commander ordered, how obedient this
      // unit was and the bonus that was added to which actions.
      report << "  order:              "
             << sim::to_string(static_cast<sim::Order>(debug.order))
             << " (obedience " << debug.obedience << ")\n";
      for (std::size_t action = 0; action < sim::kActionCount; ++action) {
        if (debug.order_bonus[action] > 0.0) {
          report << "  order term:         +"
                 << debug.order_bonus[action] << " on "
                 << sim::to_string(static_cast<sim::Action>(action)) << "\n";
        }
      }
      report << "  selected action:     " << sim::to_string(debug.chosen)
             << " (stickiness bonus +" << config.action_stickiness
             << " applies to the previous action)\n";
      if (simulation.tactics_active()) {
        report << "  tactics:             fresh contact "
               << (debug.has_fresh_contact ? "yes" : "no")
               << ", ally in need " << (debug.ally_in_need ? "yes" : "no")
               << ", flank rights " << (debug.member_may_flank ? "yes" : "no")
               << ", pursuit ticks " << debug.pursuit_ticks << "\n";
      }
    }
  }

  // Squad debug output (M6): the squad's state at the end of the run —
  // members, commander, order, cohesion. Run with a shorter --ticks to see
  // an earlier state (e.g. before vs after the commander dies).
  if (args.squad_debug_id >= 0) {
    report << "\nSquad debug (squad " << args.squad_debug_id << "):\n";
    if (!simulation.squads_active()) {
      report << "  no squads in this battle (squads off or scenario has no "
                "squad plans)\n";
    } else if (static_cast<std::size_t>(args.squad_debug_id) >=
               simulation.squads().size()) {
      report << "  no such squad id (0.."
             << simulation.squads().size() - 1 << " exist)\n";
    } else {
      const sim::Squad& squad =
          simulation.squads()[static_cast<std::size_t>(args.squad_debug_id)];
      const sim::Unit* commander = nullptr;
      if (squad.commander_index < simulation.world().units.size()) {
        commander = &simulation.world().units[squad.commander_index];
      }
      report << "  faction:             " << sim::to_string(squad.faction)
             << "\n"
             << "  formation:           "
             << sim::Simulation::formation_name(squad.formation)
             << " (spacing " << squad.spacing << ")\n"
             << "  members:             " << squad.members.size() << "\n"
             << "  commander:           ";
      if (squad.command_active && commander != nullptr) {
        report << "unit " << commander->id << (commander->alive ? "" : " (dead)")
               << (commander->routed ? " (routed)" : "") << "\n";
      } else if (commander != nullptr) {
        report << "none (command lost at tick " << squad.command_lost_tick
               << "; was unit " << commander->id << ")\n";
      } else {
        report << "none\n";
      }
      report << "  order:               "
             << sim::to_string(squad.command_active ? squad.order
                                                    : sim::Order::None)
             << "\n"
             << "  cohesion:            " << squad.cohesion << "\n"
             << "  members detail:\n";
      for (const sim::SquadMemberState& member : squad.members) {
        const sim::Unit& unit =
            simulation.world().units[member.unit_index];
        report << "    unit " << std::left << std::setw(6) << unit.id
               << std::right << ": "
               << (unit.alive ? "alive  " : "dead   ")
               << (unit.routed ? "routed " : "active ")
               << "order " << std::left << std::setw(9)
               << sim::to_string(member.order) << std::right
               << (member.in_formation ? "in formation" : "dropped    ")
               << " at (" << unit.x << ", " << unit.y << ")\n";
      }
    }
  }

  if (!red.by_type.empty() || !blue.by_type.empty()) {
    report << "Per-type stats:\n";
    const sim::FactionReport* factions[] = {&red, &blue};
    for (const sim::FactionReport* faction : factions) {
      for (const sim::TypeReport& type : faction->by_type) {
        report << "  " << std::left << std::setw(5)
               << sim::to_string(faction == &red ? sim::Faction::Red
                                                 : sim::Faction::Blue)
               << std::setw(9) << type.type_id << ":"
               << " spawned " << type.spawned << ", survivors "
               << type.survivors << ", kills " << type.kills
               << ", damage " << type.damage_dealt << std::right << "\n";
      }
    }
  }

  if (simulation.terrain() != nullptr) {
    const sim::TerrainGrid& terrain = *simulation.terrain();
    report << "Terrain:             " << terrain.map_id << " ("
           << terrain.width << "x" << terrain.height << " cells, size "
           << terrain.cell_size << ")\n";
    report << "Longest stuck stretch: " << result.longest_stuck_stretch
           << " ticks\n";
    report << "Units stuck >100 ticks: " << result.units_stuck_over_100
           << " / " << total_units << "\n";
    report << "Kills by terrain:    ";
    bool first = true;
    for (std::size_t t = 0; t < result.terrain_kills.size(); ++t) {
      if (result.terrain_kills[t] == 0) {
        continue;
      }
      if (!first) {
        report << ", ";
      }
      first = false;
      report << terrain.table->types[t].id << " "
             << result.terrain_kills[t];
    }
    if (first) {
      report << "none";
    }
    report << "\n";
  }
  report << "Execution time:      " << seconds << " s\n"
         << "Ticks per second:    " << ticks_per_second << "\n"
         << "Avg ms per tick:     " << ms_per_tick << "\n"
         << "Peak memory:         " << peak_rss_mib() << " MiB\n";

  // m13 item 4: --profile-phases. Mean per-tick wall time per pipeline
  // phase (perception = the parallel input pass; decision = the ordered
  // scoring/commit pass; resolution = flow fields/cooldowns/movement/
  // combat/deaths; commit = morale/squads/termination/sampling) plus
  // the totals. The mean-of-per-tick numbers can legitimately sum to
  // slightly less than the total mean (per-tick sum vs sum of per-tick
  // means averages identically here; both are simple sums divided by
  // the same tick count).
  if (args.profile_phases) {
    const sim::Simulation::PhaseTimings& phases =
        simulation.phase_timings();
    const double ticks =
        static_cast<double>(std::max<std::uint64_t>(phases.ticks, 1));
    const auto mean_ms = [ticks](std::uint64_t nanoseconds) {
      return static_cast<double>(nanoseconds) / ticks / 1e6;
    };
    report << "Phase profile:       (mean ms per tick, "
           << phases.ticks << " ticks, " << thread_count
           << (thread_count == 1 ? " thread)\n" : " threads)\n")
           << "  perception:        " << mean_ms(phases.perception_ns)
           << "\n"
           << "  decision:          " << mean_ms(phases.decision_ns) << "\n"
           << "  resolution:        " << mean_ms(phases.resolution_ns)
           << "\n"
           << "  commit:            " << mean_ms(phases.commit_ns) << "\n"
           << "  total:             " << mean_ms(phases.total_ns) << "\n";
  }
  report << "JSON export:         " << export_report << "\n";

  std::cout << report.str();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // --replay (M10 Part 2b): the replay file pins the battle setup; the
  // matching CLI flags are overridden from it so the re-run cannot drift.
  std::cout.imbue(std::locale::classic());
  std::cerr.imbue(std::locale::classic());

  const cli::ParseOutcome parsed = cli::parse_args(argc, argv);
  if (!parsed.ok) {
    std::cerr << "error: " << parsed.error << "\n\n";
    cli::print_usage(std::cerr);
    return 1;
  }
  if (parsed.args.help) {
    cli::print_usage(std::cout);
    return 0;
  }
  const cli::Arguments& base_args = parsed.args;
  cli::Arguments effective = base_args;
  cli::ReplayData replay_data;
  if (base_args.replay_on) {
    std::string replay_error;
    if (!cli::parse_replay(base_args.replay_path, replay_data,
                           replay_error)) {
      std::cerr << "error: " << replay_error << "\n";
      return 1;
    }
    if (replay_data.version != sim::kSimVersion) {
      std::cerr << "error: replay was recorded with sim version '"
                << replay_data.version
                << "' but this binary is sim version '" << sim::kSimVersion
                << "'; replaying across versions is not supported\n";
      return 1;
    }
    effective.scenario_path = replay_data.scenario;
    effective.mod_dir =
        replay_data.mod == "-" ? std::string{} : replay_data.mod;
    effective.seed = static_cast<std::uint32_t>(replay_data.seed);
    effective.ticks = replay_data.ticks;
    effective.variance_on = replay_data.variance == "on";
    effective.variance_explicit = true;
    effective.contact_seek_on = replay_data.contact_seek == "on";
    effective.squads_on = replay_data.squads == "on";
    effective.squads_explicit = true;
    effective.grid_mode = replay_data.neighbor == "grid";
    effective.neighbor_mode_explicit = true;
    effective.swap_sides = replay_data.swap_sides == "on";
    effective.nav_v2 = replay_data.nav == "v2";
    effective.ai_avail_v2 = replay_data.ai_avail == "v2";
    effective.end_v2 = replay_data.end == "v2";
    effective.combat_v2 = replay_data.combat == "v2";
    effective.orders_on = replay_data.orders == "on";
  }
  return run_with_replay(effective, std::move(replay_data));
}

