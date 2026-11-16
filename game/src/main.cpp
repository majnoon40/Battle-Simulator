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
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <raylib.h>

#include "cli/cli_args.hpp"
#include "game/accumulator.hpp"
#include "game/camera.hpp"
#include "game/picking.hpp"
#include "game/theme.hpp"
#include "game/view.hpp"
#include "render/battle_view.hpp"
#include "render/raster.hpp"
#include "render/ui/screens.hpp"
#include "render/ui/shell.hpp"
#include "sim/config.hpp"
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
  render::ui::App app = render::ui::sandbox_app(
      GetRenderWidth(), GetRenderHeight(), scenario_names);
  const render::ui::Theme ui_theme = render::ui::default_theme();
  render::ui::BattleView view;  // pause/speed/options/zoom from the UI
  std::string loaded_scenario = scenario_name;  // what the sim runs now

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
    simulation = sim::Simulation(config);
    camera = game::Camera::fit(simulation.world().width,
                               simulation.world().height,
                               GetRenderWidth(), GetRenderHeight());
    accumulator.reset();
    sim_seconds = 0.0;
    sim_ticks_done = 0;
    paused = false;
    finished = false;
    options.selected_unit = -1;
    options.selected_squad = -1;
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
                                     scenario_names);
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

    // --- battle-screen controls from the UI ---------------------------
    if (in_battle) {
      view = render::ui::apply_battle_frame(app, view, base_zoom);
      options = view.options;
      paused = view.paused;
      accumulator.set_speed(view.speed);
      camera.zoom = view.zoom;
      camera.clamp_zoom();
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
    if (paused && IsKeyPressed(KEY_PERIOD)) {
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
    // Click: unit first, then squad badge — never through the UI.
    if (in_battle && !pointer_over_ui &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      const Vector2 mouse = GetMousePosition();
      const std::int32_t unit = game::pick_unit(
          simulation, camera, screen_width, screen_height, mouse.x, mouse.y,
          8.0);
      if (unit >= 0) {
        options.selected_unit = unit;
        options.selected_squad =
            simulation.unit_squad_info(static_cast<std::size_t>(unit))
                .squad_id;
        // Arm the debug capture so the inspector shows live scores.
        sim::AiDebugCapture capture;
        capture.unit_id =
            simulation.world().units[static_cast<std::size_t>(unit)].id;
        capture.tick = simulation.tick_index() + 1;
        capture.explicit_tick = true;
        simulation.set_ai_debug(capture);
      } else {
        options.selected_unit = -1;
        options.selected_squad = game::pick_squad(
            simulation, camera, screen_width, screen_height, mouse.x,
            mouse.y, 10.0);
      }
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
    render::DrawList list = render::build_battle_draw_list(
        simulation, camera, options, screen_width, screen_height, stats);
    if (!in_battle) {
      list.background = game::background_color();
      list.cmds.clear();
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
