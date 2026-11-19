// battle_game — the raylib viewer window (M7), with the sandbox UI
// screens (m13 item 1): the window maps OS input to ui::Events, runs the
// pure reducer (reduce_app), draws the UI tree over the battle DrawList
// and reloads the battle when the scenario picker picks one. All policy
// lives in pure functions (render/ui/shell.cpp, game_core, render/) that
// the headless tests cover; this file only wires them to raylib, so it
// stays compile-verified (CI game-build) plus the human checklist in
// docs/viewer.md.
//
// UNVERIFIED CODE: this file needs raylib, which needs X11/GL development
// headers that this sandbox does not have; it is built only with
// -DBUILD_GAME=ON and was NOT compiled or run here. All of its logic that
// could be made pure lives in game_core (camera, accumulator, picking, HUD
// text, colors), render/ (the DrawList both backends consume) and
// render/ui/shell.cpp (the window policy), which ARE built and tested
// headlessly; this file only wires them to raylib.
//
// Controls (battle screen): Space pause, +/- speed (1,2,4,8,16x), '.'
// step one tick when paused, WASD/arrows and right-drag pan, wheel zoom,
// F fit, R restart from tick 0 with the same seed, N new seed, left-click
// a unit or squad badge to inspect, L squad links, I sight radius, O
// formation slots, T terrain, G grid, H HUD. The sim runs at 60 ticks/s
// at 1x. UI keys (Tab / Enter / Escape / arrows over a slider or open
// select) reach the screens; the pointer over UI panels belongs to the
// UI. The sim runs at 60 ticks/s at 1x.
#include <chrono>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <raylib.h>

#include "cli/cli_args.hpp"
#include "game/accumulator.hpp"
#include "cli/replay.hpp"
#include "cli/replay_runtime.hpp"
#include "cli/report.hpp"
#include "game/battle_input.hpp"
#include "game/settings.hpp"
#include "game/setup_edit.hpp"
#include "game/camera.hpp"
#include "game/picking.hpp"
#include "game/theme.hpp"
#include "game/view.hpp"
#include "render/battle_view.hpp"
#include "render/raster.hpp"
#include "render/ui/screens.hpp"
#include "render/ui/shell.hpp"
#include "sim/config.hpp"
#include "sim/version.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/squads.hpp"
#include "sim/unit_types.hpp"

namespace {

constexpr const char* kDefaultConfigPath = "data/units.json";
constexpr const char* kDefaultScenarioPath = "data/scenarios/viewer-demo.json";
constexpr const char* kDefaultPersonalitiesPath = "data/personalities.json";
constexpr const char* kDefaultMoralePath = "data/morale.json";
constexpr const char* kDefaultFormationsPath = "data/formations.json";
constexpr const char* kDefaultCommandersPath = "data/commanders.json";

std::string read_file(const std::string& path, const char* what) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    std::fprintf(stderr, "error: cannot open %s file '%s'\n", what,
                 path.c_str());
    std::exit(1);
  }
  std::string text;
  char buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    text.append(buffer, read);
  }
  std::fclose(file);
  return text;
}

// Builds the battle configuration from the same data files and arguments
// the CLI uses (see cli/src/main.cpp).
sim::SimulationConfig build_config(const cli::Arguments& args,
                                   const sim::UnitTypeTable& types,
                                   const sim::Scenario& scenario,
                                   const sim::PersonalityTable& personalities,
                                   const sim::MoraleConfig& morale,
                                   const sim::FormationConfig& formations,
                                   const sim::CommanderConfig& commanders,
                                   std::string& scenario_name) {
  const std::string scenario_path =
      args.scenario_path.empty() ? kDefaultScenarioPath : args.scenario_path;
  scenario_name = scenario_path;

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
  config.unit_types = &types;
  config.personalities = &personalities;
  config.morale = &morale;
  config.ai_mode =
      args.utility_ai ? sim::AiMode::Utility : sim::AiMode::Legacy;
  config.morale_enabled = args.morale_on && args.utility_ai;
  config.neighbor_mode =
      args.grid_mode ? sim::NeighborMode::Grid : sim::NeighborMode::Brute;
  config.grid_cell_size = args.grid_cell_size;
  config.squads_enabled = args.squads_on && args.utility_ai;
  config.formations = &formations;
  config.commanders = &commanders;

  const auto armies = sim::build_composition(scenario, types);
  if (!armies[static_cast<std::size_t>(sim::Faction::Red)].empty() &&
      !armies[static_cast<std::size_t>(sim::Faction::Blue)].empty()) {
    config.composition = armies;
    for (const sim::Faction faction : sim::kAllFactions) {
      const std::size_t index = static_cast<std::size_t>(faction);
      if (const auto& plan = scenario.squads[index]) {
        config.squad_plans[index] = sim::SquadPlan{plan->count, plan->size};
      }
    }
  } else {
    // Legacy --units split mode.
    config.total_units = args.units;
    sim::UnitStats stats = types.types[0].stats;
    config.red_stats = stats;
    config.blue_stats = stats;
  }
  return config;
}

::Color to_raylib(const game::Color& color) {
  return ::Color{color.r, color.g, color.b, color.a};
}

// Draws the same DrawList the PNG backend rasterizes.
void draw_list_raylib(const render::DrawList& list) {
  for (const render::DrawCmd& cmd : list.cmds) {
    switch (cmd.kind) {
      case render::CmdKind::Circle: {
        const bool outline_only = cmd.e > 0.5;
        if (!outline_only && cmd.c > 0.5) {
          DrawCircleV(Vector2{static_cast<float>(cmd.a),
                              static_cast<float>(cmd.b)},
                      static_cast<float>(cmd.c), to_raylib(cmd.color));
        }
        if (cmd.d > 0.5 || outline_only) {
          DrawCircleLines(static_cast<int>(cmd.a), static_cast<int>(cmd.b),
                          cmd.c, to_raylib(cmd.color2));
        }
        break;
      }
      case render::CmdKind::Triangle:
        DrawTriangle(
            Vector2{static_cast<float>(cmd.a), static_cast<float>(cmd.b)},
            Vector2{static_cast<float>(cmd.c), static_cast<float>(cmd.d)},
            Vector2{static_cast<float>(cmd.e), static_cast<float>(cmd.f)},
            to_raylib(cmd.color));
        break;
      case render::CmdKind::Line:
        DrawLineEx(Vector2{static_cast<float>(cmd.a),
                           static_cast<float>(cmd.b)},
                   Vector2{static_cast<float>(cmd.c),
                           static_cast<float>(cmd.d)},
                   static_cast<float>(cmd.e), to_raylib(cmd.color));
        break;
      case render::CmdKind::Rect:
        if (cmd.e > 0.5) {
          DrawRectangle(static_cast<int>(cmd.a), static_cast<int>(cmd.b),
                        static_cast<int>(cmd.c), static_cast<int>(cmd.d),
                        to_raylib(cmd.color));
        } else {
          Rectangle rectangle{static_cast<float>(cmd.a),
                              static_cast<float>(cmd.b),
                              static_cast<float>(cmd.c),
                              static_cast<float>(cmd.d)};
          DrawRectangleLinesEx(rectangle, 1.0f, to_raylib(cmd.color));
        }
        break;
      case render::CmdKind::Text:
        DrawText(cmd.text.c_str(), static_cast<int>(cmd.a),
                 static_cast<int>(cmd.b), static_cast<int>(cmd.c),
                 to_raylib(cmd.color));
        break;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const cli::ParseOutcome outcome = cli::parse_args(argc, argv);
  if (!outcome.ok) {
    std::fprintf(stderr, "error: %s\n", outcome.error.c_str());
    return 1;
  }
  if (outcome.args.help) {
    cli::print_usage(std::cout);
    return 0;
  }
  const cli::Arguments& args = outcome.args;

  const sim::UnitTypeTable types = sim::parse_unit_types(
      read_file(args.config_path.empty() ? kDefaultConfigPath
                                         : args.config_path,
                "unit type config"));
  const sim::Scenario scenario = sim::parse_scenario(
      read_file(args.scenario_path.empty() ? kDefaultScenarioPath
                                           : args.scenario_path,
                "scenario"));
  const sim::PersonalityTable personalities = sim::parse_personalities(
      read_file(kDefaultPersonalitiesPath, "personality table"));
  const sim::MoraleConfig morale =
      sim::parse_morale(read_file(kDefaultMoralePath, "morale config"));
  const sim::FormationConfig formations =
      sim::parse_formations(read_file(kDefaultFormationsPath, "formations"));
  const sim::CommanderConfig commanders =
      sim::parse_commanders(read_file(kDefaultCommandersPath, "commanders"));

  std::string scenario_name;
  sim::SimulationConfig config = build_config(
      args, types, scenario, personalities, morale, formations, commanders,
      scenario_name);

  InitWindow(1280, 720, "autobattle viewer");
  SetTargetFPS(60);

  sim::Simulation simulation(config);
  game::Camera camera = game::Camera::fit(
      simulation.world().width, simulation.world().height,
      GetRenderWidth(), GetRenderHeight());
  double base_zoom = camera.zoom;  // the slider's midpoint
  game::TickAccumulator accumulator(60.0, 256);
  game::ViewOptions options;
  bool paused = false;
  bool finished = false;
  double sim_seconds = 0.0;  // total time spent ticking, for the HUD tps
  std::uint64_t sim_ticks_done = 0;

  // --- the sandbox UI (m13 item 1) ------------------------------------
  // The app starts on the menu; the CLI-provided battle renders as the
  // backdrop until a scenario is picked (the picker's Start reloads the
  // battle). All policy — gating, clicks, zoom mapping — lives in
  // render/ui/shell.cpp; this loop only translates raylib input into
  // ui::Events and renders what the reducers produce.
  std::vector<std::string> scenario_names =
      render::ui::list_scenarios("data/scenarios");
  if (scenario_names.empty()) {
    scenario_names.push_back("default");
  }
  std::vector<std::string> replay_names = render::ui::list_replays(".");
  // m13 item 5c: settings (settings.json in the working directory;
  // missing file = defaults). Applied before the first frame.
  game::Settings settings;
  if (std::ifstream settings_file("settings.json"); settings_file) {
    std::string text((std::istreambuf_iterator<char>(settings_file)),
                     std::istreambuf_iterator<char>());
    std::string settings_error;
    game::Settings parsed;
    if (game::parse_settings(text, parsed, settings_error)) {
      settings = parsed;
    } else {
      std::cerr << "warning: settings.json ignored: " << settings_error
                << "\n";
    }
  }
  game::set_colorblind_palette(settings.colorblind);
  render::ui::Theme ui_theme =
      render::ui::scale_theme(render::ui::default_theme(), settings.ui_scale);
  render::ui::App app = render::ui::sandbox_app(
      GetRenderWidth(), GetRenderHeight(), scenario_names, replay_names,
      ui_theme);
  render::ui::BattleView view;  // pause/speed/options/zoom from the UI
  std::string loaded_scenario = scenario_name;  // what the sim runs now
  // m13 item 5a: multi-squad selection + click/drag bookkeeping. The
  // press positions distinguish a click from a drag (left: pick vs
  // rect-select; right: order vs pan). Pure policy lives in
  // game/battle_input (headless-tested); this loop only feeds facts.
  game::SquadSelection selection;
  bool show_minimap = true;  // the window's interactive layers (M toggles)
  Vector2 left_press{0.0f, 0.0f};
  Vector2 right_press{0.0f, 0.0f};
  bool left_pressed_battle = false;
  bool right_pressed_battle = false;
  bool left_pressed_minimap = false;
  // m13 item 5b: the end screen + replay viewer state. The window
  // records per-tick alive counts (the end-screen curves), state-hash
  // checkpoints every 25 ticks and its own issued orders (the 5a order
  // paths) so Save replay writes exactly what --record would. In
  // replay mode the accumulator stops driving the sim: , / . seek
  // (a seek is a full re-run from tick 0, like --seek), Home jumps to
  // the start, End runs to the end.
  std::vector<std::uint32_t> alive_red;
  std::vector<std::uint32_t> alive_blue;
  cli::ReplayData live_replay;
  sim::EventRecorder window_events;
  bool end_shown = false;
  bool replay_mode = false;
  cli::ReplayData replay_data;
  cli::Arguments replay_args;
  std::uint64_t replay_tick = 0;
  std::string replay_error;

  // (Re)builds the battle for a scenario stem; resets every viewer
  // state exactly like R (restart) does.
  const auto load_scenario = [&](const std::string& stem) {
    const std::string path = "data/scenarios/" + stem + ".json";
    const sim::Scenario picked =
        sim::parse_scenario(read_file(path, "scenario"));
    cli::Arguments scenario_args = args;
    scenario_args.scenario_path = path;
    config = build_config(scenario_args, types, picked, personalities,
                          morale, formations, commanders, scenario_name);
    config.events = &window_events;
    simulation = sim::Simulation(config);
    camera = game::Camera::fit(simulation.world().width,
                               simulation.world().height,
                               GetRenderWidth(), GetRenderHeight());
    accumulator.reset();
    sim_seconds = 0.0;
    sim_ticks_done = 0;
    paused = false;
    finished = false;
    end_shown = false;
    replay_mode = false;
    alive_red.clear();
    alive_blue.clear();
    live_replay = cli::ReplayData{};
    options.selected_unit = -1;
    options.selected_squad = -1;
    selection = game::SquadSelection{};
    loaded_scenario = stem;
  };

  while (!WindowShouldClose()) {
    const int screen_width = GetRenderWidth();
    const int screen_height = GetRenderHeight();

    // A resized window relayouts the screens (the widget values reset —
    // the sandbox keeps no per-size state; noted in docs/viewer.md).
    if (app.width != screen_width || app.height != screen_height) {
      app = render::ui::sandbox_app(screen_width, screen_height,
                                    scenario_names);
    }
    const bool in_battle = app.screen == render::ui::Screen::Battle;

    // m13 item 5b: refresh the replay list on entering the viewer (new
    // files may have appeared; a resize rebuild already refreshes it).
    static render::ui::Screen previous_screen = render::ui::Screen::Menu;
    if (app.screen == render::ui::Screen::Replay &&
        previous_screen != render::ui::Screen::Replay) {
      const std::vector<std::string> replay_names =
          render::ui::list_replays(".");
      const render::ui::Screen target = app.screen;
      app = render::ui::sandbox_app(screen_width, screen_height,
                                    scenario_names, replay_names, ui_theme);
      app.screen = target;
    }
    previous_screen = app.screen;

    // --- UI events: raylib input -> ui::Event -> reduce_app -----------
    {
      std::vector<render::ui::Event> events;
      const Vector2 mouse = GetMousePosition();
      render::ui::Event move;
      move.type = render::ui::Event::Type::MouseMove;
      move.pos = render::ui::Point{static_cast<int>(mouse.x),
                                   static_cast<int>(mouse.y)};
      events.push_back(move);
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        render::ui::Event down;
        down.type = render::ui::Event::Type::MouseDown;
        down.pos = move.pos;
        down.button = 0;
        events.push_back(down);
      }
      if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        render::ui::Event up;
        up.type = render::ui::Event::Type::MouseUp;
        up.pos = move.pos;
        up.button = 0;
        events.push_back(up);
      }
      const double wheel = GetMouseWheelMove();
      if (wheel != 0.0) {
        render::ui::Event scroll;
        scroll.type = render::ui::Event::Type::Wheel;
        scroll.pos = move.pos;
        scroll.delta = wheel > 0.0 ? 1 : -1;
        events.push_back(scroll);
      }
      const auto key_event = [](render::ui::Key key) {
        render::ui::Event event;
        event.type = render::ui::Event::Type::Key;
        event.key = key;
        return event;
      };
      // Arrow keys go to the UI when it owns them (menus, a focused
      // slider / open select on the battle screen); otherwise they pan.
      const render::ui::ArrowOwnership arrows =
          render::ui::ui_arrow_ownership(app);
      if (IsKeyPressed(KEY_TAB)) {
        events.push_back(key_event(render::ui::Key::Tab));
      }
      if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        events.push_back(key_event(render::ui::Key::Enter));
      }
      if (IsKeyPressed(KEY_ESCAPE)) {
        events.push_back(key_event(render::ui::Key::Escape));
      }
      if (IsKeyPressed(KEY_HOME)) {
        events.push_back(key_event(render::ui::Key::Home));
      }
      if (IsKeyPressed(KEY_END)) {
        events.push_back(key_event(render::ui::Key::End));
      }
      if (arrows.up_down) {
        if (IsKeyPressed(KEY_UP)) {
          events.push_back(key_event(render::ui::Key::Up));
        }
        if (IsKeyPressed(KEY_DOWN)) {
          events.push_back(key_event(render::ui::Key::Down));
        }
      }
      if (arrows.left_right) {
        if (IsKeyPressed(KEY_LEFT)) {
          events.push_back(key_event(render::ui::Key::Left));
        }
        if (IsKeyPressed(KEY_RIGHT)) {
          events.push_back(key_event(render::ui::Key::Right));
        }
      }
      for (const render::ui::Event& event : events) {
        app = render::ui::reduce_app(std::move(app), event,
                                     scenario_names, ui_theme);
      }
      if (app.quit) {
        break;
      }
      // The picker's Start reloads the battle with the chosen scenario
      // (once; Back to the battle keeps the running one).
      if (app.screen == render::ui::Screen::Battle &&
          !app.chosen_scenario.empty() &&
          app.chosen_scenario != loaded_scenario) {
        load_scenario(app.chosen_scenario);
        // The slider's midpoint follows the newly fitted camera.
        base_zoom = camera.zoom;
      }
    }

    // --- m13 item 5b: the replay viewer ---------------------------------
    if (app.screen == render::ui::Screen::Replay) {
      const render::ui::ReplayClicks clicks =
          render::ui::replay_clicks(app.replay);
      const auto set_status = [&app](const std::string& text) {
        for (render::ui::Widget& widget : app.replay.tree.widgets) {
          if (widget.id == render::ui::kIdReplayStatus) {
            widget.text = text;
          }
        }
      };
      const auto selected_path = [&]() -> std::string {
        if (clicks.load_stem.empty()) {
          return "";
        }
        return clicks.load_stem + ".replay";
      };
      // Load: parse, version-check, rebuild the sim from the recorded
      // setup and switch to the battle screen in replay mode at tick 0.
      if (clicks.load && !selected_path().empty()) {
        std::string error;
        cli::ReplayData data;
        if (!cli::parse_replay(selected_path(), data, error)) {
          set_status("parse FAILED: " + error);
        } else if (data.version != sim::kSimVersion) {
          set_status("version mismatch: recorded " + data.version +
                     ", binary " + sim::kSimVersion);
        } else if (data.mod != "-") {
          set_status("replay needs a mod; the window loads no mods");
        } else {
          replay_data = data;
          replay_args = cli::arguments_from_replay(replay_data);
          const sim::Scenario scenario =
              sim::parse_scenario(read_file(replay_data.scenario,
                                            "scenario"));
          std::string name;
          config = build_config(replay_args, types, scenario,
                                personalities, morale, formations,
                                commanders, name);
          config.orders_enabled = replay_data.orders == "on";
          simulation = sim::Simulation(config);
          replay_mode = true;
          replay_tick = 0;
          finished = simulation.finished();
          camera = game::Camera::fit(simulation.world().width,
                                     simulation.world().height,
                                     screen_width, screen_height);
          base_zoom = camera.zoom;
          accumulator.reset();
          end_shown = false;
          alive_red.clear();
          alive_blue.clear();
          selection = game::SquadSelection{};
          app.screen = render::ui::Screen::Battle;
        }
      } else if (clicks.verify && !selected_path().empty()) {
        std::string error;
        cli::ReplayData data;
        if (!cli::parse_replay(selected_path(), data, error)) {
          set_status("parse FAILED: " + error);
        } else if (data.version != sim::kSimVersion) {
          set_status("version mismatch: recorded " + data.version);
        } else if (data.mod != "-") {
          set_status("replay needs a mod; the window loads no mods");
        } else {
          const cli::Arguments verify_args =
              cli::arguments_from_replay(data);
          const sim::Scenario scenario =
              sim::parse_scenario(read_file(data.scenario, "scenario"));
          std::string name;
          sim::SimulationConfig verify_config = build_config(
              verify_args, types, scenario, personalities, morale,
              formations, commanders, name);
          verify_config.orders_enabled = data.orders == "on";
          sim::Simulation verify(verify_config);
          cli::ReplayDriver driver(verify, data.interventions,
                                   data.order_interventions);
          const cli::ReplayDriver::RunResult run =
              driver.run(0, nullptr, &data);
          if (run.mismatch) {
            set_status("MISMATCH at tick " +
                       std::to_string(run.mismatch_tick));
          } else if (run.checkpoints_verified < data.hashes.size()) {
            set_status("FAILED: replay ended at t" +
                       std::to_string(run.last_tick) + " with " +
                       std::to_string(data.hashes.size() -
                                      run.checkpoints_verified) +
                       " unverified checkpoints");
          } else {
            set_status("verified: " +
                       std::to_string(data.hashes.size()) +
                       " checkpoints over " +
                       std::to_string(run.last_tick) + " ticks");
          }
        }
      }
    }

    // --- m13 item 5c: the army editor -----------------------------------
    static game::SetupEdit setup_edit;
    static bool setup_loaded = false;
    if (app.screen == render::ui::Screen::Setup) {
      // Entering the editor (or after a reload): build the screen for
      // the loaded scenario's types and snapshot the current armies.
      if (!setup_loaded) {
        setup_loaded = true;
        std::vector<std::string> type_names;
        for (const sim::UnitType& type : types.types) {
          type_names.push_back(type.id);
        }
        app.setup = render::ui::setup_state(screen_width, screen_height,
                                            ui_theme, type_names);
        const sim::Scenario current = sim::parse_scenario(
            read_file("data/scenarios/" + loaded_scenario + ".json",
                      "scenario"));
        setup_edit = game::setup_from_scenario(
            current, types, sim::build_composition(current, types));
        // Fill the count labels with the edited faction's live counts
        // (the stepper branch below rewrites them after every step).
        std::size_t initial_faction = 0;
        for (const render::ui::Widget& widget : app.setup.tree.widgets) {
          if (widget.id == render::ui::kIdSetupFaction) {
            initial_faction = widget.selected == 0 ? 0 : 1;
          }
        }
        for (render::ui::Widget& widget : app.setup.tree.widgets) {
          if (widget.id.rfind("count-", 0) == 0) {
            const int index = types.index_of(widget.id.substr(6));
            if (index >= 0) {
              widget.text = std::to_string(
                  setup_edit.counts[initial_faction]
                                [static_cast<std::size_t>(index)]);
            }
          }
        }
      }
      const render::ui::SetupClicks clicks =
          render::ui::setup_clicks(app.setup);
      std::size_t edited_faction = 0;
      for (const render::ui::Widget& widget : app.setup.tree.widgets) {
        if (widget.id == render::ui::kIdSetupFaction) {
          edited_faction = widget.selected == 0 ? 0 : 1;
        }
      }
      int stepped_type = -1;
      int step_delta = 0;
      if (!clicks.plus_type.empty()) {
        stepped_type = types.index_of(clicks.plus_type);
        step_delta = 1;
      } else if (!clicks.minus_type.empty()) {
        stepped_type = types.index_of(clicks.minus_type);
        step_delta = -1;
      }
      if (stepped_type >= 0) {
        static_cast<void>(game::setup_step(
            setup_edit, edited_faction,
            static_cast<std::size_t>(stepped_type), step_delta, 20000));
        for (render::ui::Widget& widget : app.setup.tree.widgets) {
          if (widget.id.rfind("count-", 0) == 0) {
            const int index = types.index_of(widget.id.substr(6));
            if (index >= 0) {
              widget.text = std::to_string(
                  setup_edit.counts[edited_faction]
                                [static_cast<std::size_t>(index)]);
            }
          }
        }
      } else if (clicks.save || clicks.start) {
        const sim::Scenario base = sim::parse_scenario(
            read_file("data/scenarios/" + loaded_scenario + ".json",
                      "scenario"));
        const sim::Scenario edited =
            game::to_scenario(base, setup_edit, types);
        const std::string stem = loaded_scenario + "-setup";
        const std::string path = "data/scenarios/" + stem + ".json";
        std::ofstream out(path);
        out << sim::write_scenario(edited);
        if (clicks.start && out.good()) {
          load_scenario(stem);
          scenario_names = render::ui::list_scenarios("data/scenarios");
          app.screen = render::ui::Screen::Battle;
        }
      }
    } else {
      setup_loaded = false;  // re-entering rebuilds from the live scenario
    }

    // --- m13 item 5c: the settings screen --------------------------------
    if (app.screen == render::ui::Screen::Settings &&
        render::ui::settings_apply_clicked(app.settings)) {
      settings = render::ui::settings_from(app.settings);
      game::set_colorblind_palette(settings.colorblind);
      ui_theme = render::ui::scale_theme(render::ui::default_theme(),
                                         settings.ui_scale);
      SetWindowSize(settings.width, settings.height);
      if (settings.vsync) {
        SetWindowState(FLAG_VSYNC_HINT);
      } else {
        ClearWindowState(FLAG_VSYNC_HINT);
      }
      const bool fullscreen_now = IsWindowFullscreen();
      if (settings.fullscreen != fullscreen_now) {
        ToggleFullscreen();
      }
      std::ofstream out("settings.json");
      out << game::write_settings(settings);
      app = render::ui::sandbox_app(GetRenderWidth(), GetRenderHeight(),
                                    scenario_names, replay_names, ui_theme);
      app.screen = render::ui::Screen::Settings;
    }

    // --- battle-screen controls from the UI ---------------------------
    if (in_battle) {
      view = render::ui::apply_battle_frame(app, view, base_zoom);
      if (!replay_mode) {
        options = view.options;
        paused = view.paused;
        accumulator.set_speed(view.speed);
      } else {
        // Replay mode: the scrub keys drive the sim; the playback
        // widgets are ignored (seeking re-runs from tick 0, so free
        // running would desync from the recorded checkpoints).
        options = view.options;
        paused = true;
      }
      camera.zoom = view.zoom;
      camera.clamp_zoom();
    }

    // --- m13 item 5b: the end screen ------------------------------------
    if (in_battle && simulation.finished() && !end_shown) {
      end_shown = true;
      app.screen = render::ui::Screen::End;
      const sim::BattleResult result = simulation.result();
      std::string line = "draw";
      if (result.has_winner) {
        line = std::string(sim::to_string(result.winner)) + " wins - " +
               sim::to_string(result.reason) + " (t" +
               std::to_string(result.ticks_elapsed) + ")";
      } else {
        line = std::string("draw - ") + sim::to_string(result.reason) +
               " (t" + std::to_string(result.ticks_elapsed) + ")";
      }
      const auto faction_index = [](sim::Faction faction) {
        return static_cast<std::size_t>(faction);
      };
      const std::string casualties =
          "Red " +
          std::to_string(result.factions[faction_index(sim::Faction::Red)]
                             .deaths) +
          " dead / " +
          std::to_string(result.factions[faction_index(sim::Faction::Red)]
                             .survivors) +
          " left   |   Blue " +
          std::to_string(
              result.factions[faction_index(sim::Faction::Blue)].deaths) +
          " dead / " +
          std::to_string(
              result.factions[faction_index(sim::Faction::Blue)].survivors) +
          " left";
      for (render::ui::Widget& widget : app.end.tree.widgets) {
        if (widget.id == render::ui::kIdEndResult) {
          widget.text = line;
        } else if (widget.id == render::ui::kIdEndCasualties) {
          widget.text = casualties;
        }
      }
    }
    if (app.screen == render::ui::Screen::End) {
      const render::ui::EndClicks clicks = render::ui::end_clicks(app.end);
      if (clicks.rematch) {
        load_scenario(loaded_scenario);
        app.screen = render::ui::Screen::Battle;
      } else if (clicks.save_replay) {
        live_replay.version = sim::kSimVersion;
        live_replay.scenario =
            "data/scenarios/" + loaded_scenario + ".json";
        live_replay.mod = "-";
        live_replay.seed = config.seed;
        live_replay.ticks = config.max_ticks;
        live_replay.variance = config.tactics_enabled ? "on" : "off";
        live_replay.contact_seek =
            config.contact_seek_enabled ? "on" : "off";
        live_replay.neighbor =
            config.ai_mode == sim::AiMode::Utility ? "grid" : "brute";
        live_replay.squads = config.squads_enabled ? "on" : "off";
        live_replay.swap_sides = "off";
        live_replay.nav = config.nav_v2 ? "v2" : "v1";
        live_replay.ai_avail = config.ai_avail_v2 ? "v2" : "v1";
        live_replay.end = config.end_v2 ? "v2" : "v1";
        live_replay.combat = config.combat_v2 ? "v2" : "v1";
        live_replay.orders = config.orders_enabled ? "on" : "off";
        const std::string path = loaded_scenario + ".replay";
        if (cli::write_replay(path, live_replay)) {
          for (render::ui::Widget& widget : app.end.tree.widgets) {
            if (widget.id == render::ui::kIdEndResult) {
              widget.text = "replay saved: " + path;
            }
          }
        } else {
          for (render::ui::Widget& widget : app.end.tree.widgets) {
            if (widget.id == render::ui::kIdEndResult) {
              widget.text = "replay save FAILED: " + path;
            }
          }
        }
      } else if (clicks.save_report) {
        cli::ReportMeta meta;
        meta.scenario = scenario_name;
        meta.seed = config.seed;
        const std::string markdown = cli::generate_report_markdown(
            simulation, &window_events, types, meta);
        const std::string path = loaded_scenario + "-report.md";
        std::ofstream out(path);
        out << markdown;
        for (render::ui::Widget& widget : app.end.tree.widgets) {
          if (widget.id == render::ui::kIdEndResult) {
            widget.text = out.good() ? "report saved: " + path
                                     : "report save FAILED: " + path;
          }
        }
      }
    }
    const render::ui::Point pointer{
        static_cast<int>(GetMousePosition().x),
        static_cast<int>(GetMousePosition().y)};
    const bool pointer_over_ui = render::ui::ui_covers(app, pointer);

    // --- input -----------------------------------------------------------
    if (in_battle) {
    if (IsKeyPressed(KEY_SPACE)) {
      paused = !paused;
    }
    if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) {
      accumulator.set_speed(game::TickAccumulator::speed_up(accumulator.speed()));
    }
    if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) {
      accumulator.set_speed(
          game::TickAccumulator::speed_down(accumulator.speed()));
    }
    if (paused && IsKeyPressed(KEY_PERIOD) && !replay_mode) {
      simulation.tick();  // step one tick
      ++sim_ticks_done;
    }
    if (IsKeyPressed(KEY_F)) {
      camera = game::Camera::fit(simulation.world().width,
                                 simulation.world().height, screen_width,
                                 screen_height);
    }
    if (IsKeyPressed(KEY_R)) {  // restart, same seed
      simulation = sim::Simulation(config);
      accumulator.reset();
      sim_seconds = 0.0;
      sim_ticks_done = 0;
      paused = false;
      finished = false;
      options.selected_unit = -1;
      options.selected_squad = -1;
      selection = game::SquadSelection{};
    }
    if (IsKeyPressed(KEY_N)) {  // new seed
      const auto now = std::chrono::steady_clock::now().time_since_epoch();
      config.seed = static_cast<std::uint64_t>(now.count()) & 0xFFFFFFFFFFFF;
      simulation = sim::Simulation(config);
      accumulator.reset();
      sim_seconds = 0.0;
      sim_ticks_done = 0;
      paused = false;
      finished = false;
      options.selected_unit = -1;
      options.selected_squad = -1;
      selection = game::SquadSelection{};
    }
    if (IsKeyPressed(KEY_L)) {
      options.squad_links = !options.squad_links;
    }
    if (IsKeyPressed(KEY_I)) {
      options.show_sight_radius = !options.show_sight_radius;
    }
    if (IsKeyPressed(KEY_O)) {
      options.formation_slots = !options.formation_slots;
    }
    if (IsKeyPressed(KEY_G)) {
      options.show_grid = !options.show_grid;
    }
    if (IsKeyPressed(KEY_T)) {
      options.show_terrain = !options.show_terrain;
    }
    if (IsKeyPressed(KEY_H)) {
      options.show_hud = !options.show_hud;
    }
    // m13 item 5b: replay scrubbing. A seek is a full re-run from
    // tick 0 (the --seek semantics), driven through the shared
    // ReplayDriver so interventions and recorded orders land exactly
    // as recorded. , / . step 25 ticks, Home restarts, End finishes.
    if (replay_mode) {
      const auto seek = [&](std::uint64_t target) {
        simulation = sim::Simulation(config);
        cli::ReplayDriver driver(simulation, replay_data.interventions,
                                 replay_data.order_interventions);
        const cli::ReplayDriver::RunResult run =
            driver.run(target, nullptr, nullptr);
        replay_tick = simulation.world().tick_count;
        finished = simulation.finished();
        alive_red.clear();
        alive_blue.clear();
        selection = game::SquadSelection{};
      };
      std::uint64_t target = 0;
      bool seek_now = false;
      if (IsKeyPressed(KEY_COMMA)) {
        target = replay_tick > 25 ? replay_tick - 25 : 1;
        seek_now = true;
      } else if (IsKeyPressed(KEY_PERIOD)) {
        target = replay_tick + 25;
        seek_now = true;
      } else if (IsKeyPressed(KEY_HOME)) {
        target = 1;
        seek_now = true;
      } else if (IsKeyPressed(KEY_END)) {
        target = 0;  // run to the end
        seek_now = true;
      }
      if (seek_now) {
        seek(target);
      }
    }
    // m13 item 5a: order hotkeys on the multi-squad selection
    // (1/2/3 = Hold/Retreat/Regroup) and the minimap toggle. The
    // intents route through the pure game:: policy and land in the sim
    // through issue_order, exactly like --script. Never in replay mode
    // (extra orders would desync the recorded checkpoints).
    if (IsKeyPressed(KEY_ONE) && !replay_mode) {
      const game::OrderIntent intent =
          game::order_from_hotkey(selection, game::OrderType::Hold);
      static_cast<void>(game::apply_order(simulation, intent));
    }
    if (IsKeyPressed(KEY_TWO) && !replay_mode) {
      const game::OrderIntent intent =
          game::order_from_hotkey(selection, game::OrderType::Retreat);
      static_cast<void>(game::apply_order(simulation, intent));
    }
    if (IsKeyPressed(KEY_THREE) && !replay_mode) {
      const game::OrderIntent intent =
          game::order_from_hotkey(selection, game::OrderType::Regroup);
      static_cast<void>(game::apply_order(simulation, intent));
    }
    if (IsKeyPressed(KEY_M)) {
      show_minimap = !show_minimap;
    }
    options.show_minimap = show_minimap;
    options.show_order_markers = true;
    }
    // Pan: held keys and right-drag (pixels per second at 1x speed feel).
    // Arrow keys pan only when the UI does not own them.
    {
      const render::ui::ArrowOwnership arrows =
          render::ui::ui_arrow_ownership(app);
      const double dt = GetFrameTime();
      double pan_x = 0.0;
      double pan_y = 0.0;
      if (IsKeyDown(KEY_W) || (in_battle && !arrows.up_down &&
                               IsKeyDown(KEY_UP))) {
        pan_y -= 600.0 * dt;
      }
      if (IsKeyDown(KEY_S) || (in_battle && !arrows.up_down &&
                               IsKeyDown(KEY_DOWN))) {
        pan_y += 600.0 * dt;
      }
      if (IsKeyDown(KEY_A) || (in_battle && !arrows.left_right &&
                               IsKeyDown(KEY_LEFT))) {
        pan_x -= 600.0 * dt;
      }
      if (IsKeyDown(KEY_D) || (in_battle && !arrows.left_right &&
                               IsKeyDown(KEY_RIGHT))) {
        pan_x += 600.0 * dt;
      }
      if (in_battle && IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        const Vector2 delta = GetMouseDelta();
        pan_x += delta.x;
        pan_y += delta.y;
      }
      if (pan_x != 0.0 || pan_y != 0.0) {
        camera.pan(pan_x, pan_y);
      }
    }
    // Zoom: wheel about the cursor — the UI's slider follows the camera
    // (slider_from_zoom is the exact inverse of the shell's mapping).
    {
      const double wheel = GetMouseWheelMove();
      if (wheel != 0.0 && in_battle && !pointer_over_ui) {
        const Vector2 mouse = GetMousePosition();
        camera.zoom_at(mouse.x, mouse.y, std::pow(1.15, wheel),
                       screen_width, screen_height);
        for (render::ui::Widget& widget : app.battle.tree.widgets) {
          if (widget.id == "zoom") {
            widget.value =
                render::ui::slider_from_zoom(camera.zoom, base_zoom);
          }
        }
      }
    }
    // m13 item 5a: left press/release. A press inside the minimap jumps
    // the camera there on release; a drag (> 6 px) rect-selects every
    // Red squad touched; a plain click picks a unit first, then a squad
    // badge (the m11 rule), and the multi-selection follows the pick.
    // Never through the UI chrome.
    if (in_battle && !pointer_over_ui &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      left_press = GetMousePosition();
      left_pressed_battle = true;
      const game::MinimapGeometry geometry = game::minimap_geometry(
          screen_width, screen_height, simulation.world().width,
          simulation.world().height);
      left_pressed_minimap = game::minimap_covers(
          geometry, static_cast<int>(left_press.x),
          static_cast<int>(left_press.y));
    }
    if (in_battle && left_pressed_battle &&
        IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
      const Vector2 mouse = GetMousePosition();
      const float dx = mouse.x - left_press.x;
      const float dy = mouse.y - left_press.y;
      const bool drag = dx * dx + dy * dy > 36.0f;  // > 6 px
      const bool shift =
          IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
      if (left_pressed_minimap) {
        const game::MinimapGeometry geometry = game::minimap_geometry(
            screen_width, screen_height, simulation.world().width,
            simulation.world().height);
        camera.center_x = game::world_x_from_minimap(
            geometry, static_cast<double>(mouse.x));
        camera.center_y = game::world_y_from_minimap(
            geometry, static_cast<double>(mouse.y));
      } else if (drag) {
        selection = game::select_squad_rect(
            simulation, selection,
            camera.world_x(left_press.x, screen_width),
            camera.world_y(left_press.y, screen_height),
            camera.world_x(mouse.x, screen_width),
            camera.world_y(mouse.y, screen_height), shift);
        options.selected_unit = -1;
        options.selected_squad =
            selection.empty() ? -1
                              : static_cast<std::int32_t>(
                                    selection.squads.front());
      } else {
        const std::int32_t unit = game::pick_unit(
            simulation, camera, screen_width, screen_height, mouse.x,
            mouse.y, 8.0);
        if (unit >= 0) {
          options.selected_unit = unit;
          const auto info =
              simulation.unit_squad_info(static_cast<std::size_t>(unit));
          options.selected_squad = info.squad_id;
          selection = game::select_squad_click(
              selection, info.squad_id, shift);
          // Arm the debug capture so the inspector shows live scores.
          sim::AiDebugCapture capture;
          capture.unit_id =
              simulation.world().units[static_cast<std::size_t>(unit)].id;
          capture.tick = simulation.tick_index() + 1;
          capture.explicit_tick = true;
          simulation.set_ai_debug(capture);
        } else {
          options.selected_unit = -1;
          const std::int32_t squad = game::pick_squad(
              simulation, camera, screen_width, screen_height, mouse.x,
              mouse.y, 10.0);
          options.selected_squad = squad;
          selection = game::select_squad_click(selection, squad, shift);
        }
      }
      left_pressed_battle = false;
      left_pressed_minimap = false;
    }
    // m13 item 5a: right-click without a drag issues MoveTo (shift =
    // AttackMove) to the world point on the whole selection; a right
    // drag still pans (the pan block above runs on the held button).
    if (in_battle && !pointer_over_ui &&
        IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
      right_press = GetMousePosition();
      right_pressed_battle = true;
    }
    if (in_battle && right_pressed_battle &&
        IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
      const Vector2 mouse = GetMousePosition();
      const float dx = mouse.x - right_press.x;
      const float dy = mouse.y - right_press.y;
      if (dx * dx + dy * dy <= 36.0f) {  // a click, not a pan drag
        if (!replay_mode) {
          const bool attack =
              IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
          const game::OrderIntent intent = game::order_from_right_click(
              selection, attack,
              camera.world_x(mouse.x, screen_width),
              camera.world_y(mouse.y, screen_height));
          static_cast<void>(game::apply_order(simulation, intent));
        }
      }
      right_pressed_battle = false;
    }

    // --- simulation ------------------------------------------------------
    bool limited = false;
    if (in_battle && !paused && !finished) {
      // Keep the capture on the selected unit for the coming tick.
      if (options.selected_unit >= 0) {
        sim::AiDebugCapture capture;
        capture.unit_id =
            simulation.world()
                .units[static_cast<std::size_t>(options.selected_unit)]
                .id;
        capture.tick = simulation.tick_index() + 1;
        capture.explicit_tick = true;
        simulation.set_ai_debug(capture);
      }
      const int ticks = accumulator.advance(GetFrameTime(), limited);
      const double before = GetTime();
      for (int i = 0; i < ticks; ++i) {
        simulation.tick();
        // m13 item 5b: the end-screen curves + the replay checkpoints
        // (every 25th tick and the final one — the --record rule).
        alive_red.push_back(simulation.world().alive_count(
            sim::Faction::Red));
        alive_blue.push_back(simulation.world().alive_count(
            sim::Faction::Blue));
        const std::uint64_t tick = simulation.world().tick_count;
        if (tick % 25 == 0 || simulation.finished()) {
          live_replay.hashes.emplace_back(tick, simulation.state_hash());
        }
      }
      sim_seconds += GetTime() - before;
      sim_ticks_done += static_cast<std::uint64_t>(ticks);
      if (simulation.finished()) {
        finished = true;
      }
    }

    // --- draw ------------------------------------------------------------
    // Live playback stats go straight into the HUD text (battle screen
    // only; the menus draw over a plain background, exactly like the
    // --ui-script harness).
    render::HudStats stats;
    stats.fps = GetFPS();
    stats.sim_tps =
        sim_seconds > 0.0
            ? static_cast<double>(sim_ticks_done) / sim_seconds
            : 0.0;
    stats.speed = accumulator.speed();
    stats.limited = limited;
    stats.scenario_name = scenario_name;
    options.selected_squads.clear();
    for (std::uint32_t id : selection.squads) {
      options.selected_squads.push_back(static_cast<std::int32_t>(id));
    }
    render::DrawList list = render::build_battle_draw_list(
        simulation, camera, options, screen_width, screen_height, stats);
    // The menu / picker / replay-list screens draw over a plain
    // background; the end screen keeps the final battle frame under
    // its panel and adds the casualty curves (m13 item 5b).
    if (!in_battle && app.screen != render::ui::Screen::End) {
      list.background = game::background_color();
      list.cmds.clear();
    }
    if (app.screen == render::ui::Screen::Setup) {
      // The battlefield preview: the field, the default side bands and
      // both anchors (the edited faction's at full alpha).
      const double fx = static_cast<double>(screen_width) / 2.0 - 200.0;
      const double fy = 120.0;
      const double fw = 400.0;
      const double fh = 400.0;
      const double world = std::max(simulation.world().width,
                                    simulation.world().height);
      const double scale = fw / world;
      const auto to_screen = [&](double wx, double wy) {
        return std::pair<double, double>{
            fx + wx * scale + (fw - simulation.world().width * scale) / 2.0,
            fy + wy * scale +
                (fh - simulation.world().height * scale) / 2.0};
      };
      list.rect(fx, fy, fw, fh, game::Color{24, 30, 38, 255}, true);
      list.rect(fx, fy, fw, fh, game::text_color(), false);
      const double band_px = 0.15 * simulation.world().width * scale;
      game::Color west = game::faction_color(sim::Faction::Red);
      west.a = 36;
      game::Color east = game::faction_color(sim::Faction::Blue);
      east.a = 36;
      list.rect(fx, fy, band_px, fh, west, true);
      list.rect(fx + fw - band_px, fy, band_px, fh, east, true);
      for (std::size_t faction = 0; faction < 2; ++faction) {
        if (const auto& anchor = setup_edit.anchors[faction]) {
          const auto [ax, ay] = to_screen(anchor->x, anchor->y);
          game::Color color = game::faction_color(
              faction == 0 ? sim::Faction::Red : sim::Faction::Blue);
          color.a = 220;
          list.circle(ax, ay, 5.0, color, game::Color{});
          list.text(ax + 8.0, ay - 3.0, 7,
                    faction == 0 ? "red anchor" : "blue anchor",
                    game::text_color());
        }
      }
    }
    if (app.screen == render::ui::Screen::End) {
      render::draw_end_curves(list, 8.0, 64.0,
                              static_cast<double>(screen_width) / 2.0 - 16.0,
                              220.0, alive_red, alive_blue);
    }
    if (in_battle && replay_mode) {
      for (render::ui::Widget& widget : app.battle.tree.widgets) {
        if (widget.id == "status") {
          widget.text = "replay t" + std::to_string(replay_tick) +
                        " / " +
                        std::to_string(simulation.world().tick_count) +
                        "  (, . Home End seek)";
        }
      }
    }
    render::ui::draw_ui(app.active(), ui_theme, list);

    BeginDrawing();
    ClearBackground(to_raylib(list.background));
    draw_list_raylib(list);
    if (in_battle && finished) {
      DrawText("battle over - R restart, N new seed", 8,
               screen_height - 20, 14, LIGHTGRAY);
    }
    EndDrawing();
  }
  CloseWindow();
  return 0;
}
