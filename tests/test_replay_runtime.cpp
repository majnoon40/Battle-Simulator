// m13 item 5b — the shared replay driver, the replay->arguments
// mapping, the replay listing, the end/replay screen clicks and
// navigation, and the end-screen casualty curves.
#include "cli/replay.hpp"
#include "cli/replay_runtime.hpp"
#include "render/battle_view.hpp"
#include "render/ui/screens.hpp"
#include "render/ui/shell.hpp"
#include "sim/simulation.hpp"
#include "sim/unit_types.hpp"
#include "sim/version.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);

sim::UnitTypeTable squad_types() {
  return sim::parse_unit_types(R"json([
    {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0,
     "range": 2.0, "cooldown": 5, "sight_radius": 40.0},
    {"id": "commander", "health": 150.0, "damage": 8.0, "speed": 1.8,
     "range": 2.0, "cooldown": 10, "sight_radius": 60.0,
     "leadership_radius": 120.0}
  ])json");
}

sim::SimulationConfig squad_config(const sim::UnitTypeTable& types,
                                   std::uint32_t squads,
                                   std::uint32_t size) {
  sim::SimulationConfig config;
  config.seed = 7;
  config.max_ticks = 300;
  config.battlefield_width = 1000.0;
  config.battlefield_height = 1000.0;
  config.ai_mode = sim::AiMode::Utility;
  config.unit_types = &types;
  config.squads_enabled = true;
  const int infantry = types.index_of("infantry");
  const int commander = types.index_of("commander");
  config.composition[kRed].push_back(
      sim::TypeCount{static_cast<std::uint32_t>(commander), squads});
  config.composition[kRed].push_back(
      sim::TypeCount{static_cast<std::uint32_t>(infantry), squads * size});
  config.composition[static_cast<std::size_t>(sim::Faction::Blue)].push_back(
      sim::TypeCount{static_cast<std::uint32_t>(commander), squads});
  config.composition[static_cast<std::size_t>(sim::Faction::Blue)].push_back(
      sim::TypeCount{static_cast<std::uint32_t>(infantry), squads * size});
  config.squad_plans[kRed] = sim::SquadPlan{squads, size};
  config.squad_plans[static_cast<std::size_t>(sim::Faction::Blue)] =
      sim::SquadPlan{squads, size};
  return config;
}

std::int32_t find_id(const render::ui::State& state,
                     const std::string& id) {
  for (std::size_t i = 0; i < state.tree.size(); ++i) {
    if (state.tree.at(i).id == id) {
      return static_cast<std::int32_t>(i);
    }
  }
  return -1;
}

render::ui::Point center_of(const render::ui::State& state,
                            const std::string& id) {
  const std::int32_t index = find_id(state, id);
  EXPECT_GE(index, 0) << "widget " << id << " missing";
  const render::ui::Rect& rect =
      state.tree.at(static_cast<std::size_t>(index)).rect;
  return render::ui::Point{rect.x + rect.w / 2, rect.y + rect.h / 2};
}

const std::vector<std::string>& test_scenarios() {
  static const std::vector<std::string> scenarios{"default", "squads"};
  return scenarios;
}

render::ui::App click_at(render::ui::App app, render::ui::Point where) {
  const auto down = [&where] {
    render::ui::Event event;
    event.type = render::ui::Event::Type::MouseDown;
    event.pos = where;
    event.button = 0;
    return event;
  };
  const auto up = [&where] {
    render::ui::Event event;
    event.type = render::ui::Event::Type::MouseUp;
    event.pos = where;
    event.button = 0;
    return event;
  };
  app = render::ui::reduce_app(std::move(app), down(), test_scenarios());
  app = render::ui::reduce_app(std::move(app), up(), test_scenarios());
  return app;
}

}  // namespace

// ---------------------------------------------------------------------------
// The shared replay driver.

TEST(ReplayDriverTest, RecordThenVerifyRoundTrips) {
  const sim::UnitTypeTable types = squad_types();
  sim::Simulation recording(squad_config(types, 2, 5));
  const std::vector<cli::Intervention> no_interventions;
  const std::vector<cli::OrderIntervention> no_orders;
  cli::ReplayDriver record_driver(recording, no_interventions, no_orders);
  cli::ReplayData data;
  const cli::ReplayDriver::RunResult record =
      record_driver.run(0, &data, nullptr);
  EXPECT_FALSE(record.mismatch);
  EXPECT_GT(data.hashes.size(), 0u);
  EXPECT_EQ(data.hashes.front().first, 25u);  // checkpoints every 25 ticks

  // Re-run from a fresh simulation and verify every checkpoint.
  sim::Simulation replay(squad_config(types, 2, 5));
  cli::ReplayDriver verify_driver(replay, no_interventions, no_orders);
  const cli::ReplayDriver::RunResult verify =
      verify_driver.run(0, nullptr, &data);
  EXPECT_FALSE(verify.mismatch);
  EXPECT_EQ(verify.checkpoints_verified, data.hashes.size());
}

TEST(ReplayDriverTest, TamperedHashFailsAtItsTick) {
  const sim::UnitTypeTable types = squad_types();
  sim::Simulation recording(squad_config(types, 2, 5));
  const std::vector<cli::Intervention> no_interventions;
  const std::vector<cli::OrderIntervention> no_orders;
  cli::ReplayData data;
  cli::ReplayDriver(recording, no_interventions, no_orders)
      .run(0, &data, nullptr);
  ASSERT_GT(data.hashes.size(), 2u);
  const std::uint64_t tampered_tick = data.hashes[1].first;
  data.hashes[1].second[0] = data.hashes[1].second[0] == 'a' ? 'b' : 'a';

  sim::Simulation replay(squad_config(types, 2, 5));
  cli::ReplayDriver verify_driver(replay, no_interventions, no_orders);
  const cli::ReplayDriver::RunResult verify =
      verify_driver.run(0, nullptr, &data);
  EXPECT_TRUE(verify.mismatch);
  EXPECT_EQ(verify.mismatch_tick, tampered_tick);
}

TEST(ReplayDriverTest, SeekStopsAtTheTarget) {
  const sim::UnitTypeTable types = squad_types();
  sim::Simulation simulation(squad_config(types, 2, 5));
  const std::vector<cli::Intervention> no_interventions;
  const std::vector<cli::OrderIntervention> no_orders;
  cli::ReplayDriver driver(simulation, no_interventions, no_orders);
  const cli::ReplayDriver::RunResult run = driver.run(40, nullptr, nullptr);
  EXPECT_EQ(run.last_tick, 40u);
  EXPECT_EQ(simulation.world().tick_count, 40u);
}

TEST(ReplayDriverTest, OrdersAreAppliedAtTheirTick) {
  const sim::UnitTypeTable types = squad_types();
  sim::Simulation simulation(squad_config(types, 2, 5));
  const std::vector<cli::Intervention> no_interventions;
  std::vector<cli::OrderIntervention> orders(1);
  orders[0].tick = 5;
  orders[0].red = true;
  orders[0].squads_all = true;
  orders[0].type = sim::PlayerOrderType::Hold;
  cli::ReplayDriver driver(simulation, no_interventions, orders);
  driver.run(60, nullptr, nullptr);
  EXPECT_EQ(driver.orders_applied(), 1u);
  bool delivered = false;
  for (const sim::Squad& squad : simulation.squads()) {
    if (squad.player_order_active) {
      delivered = true;
      break;
    }
  }
  EXPECT_TRUE(delivered) << "the recorded Hold order never delivered";
}

// ---------------------------------------------------------------------------
// The replay -> arguments mapping.

TEST(ReplayArgumentsTest, MapsEveryRecordedField) {
  cli::ReplayData data;
  data.version = sim::kSimVersion;
  data.scenario = "data/scenarios/squads.json";
  data.mod = "-";
  data.seed = 99;
  data.ticks = 1234;
  data.variance = "off";
  data.contact_seek = "off";
  data.neighbor = "brute";
  data.squads = "off";
  data.swap_sides = "on";
  data.nav = "v1";
  data.ai_avail = "v1";
  data.end = "v1";
  data.combat = "v1";
  data.orders = "off";
  const cli::Arguments args = cli::arguments_from_replay(data);
  EXPECT_EQ(args.scenario_path, "data/scenarios/squads.json");
  EXPECT_TRUE(args.mod_dir.empty());
  EXPECT_EQ(args.seed, 99u);
  EXPECT_EQ(args.ticks, 1234u);
  EXPECT_FALSE(args.variance_on);
  EXPECT_FALSE(args.contact_seek_on);
  EXPECT_FALSE(args.grid_mode);
  EXPECT_FALSE(args.squads_on);
  EXPECT_TRUE(args.swap_sides);
  EXPECT_FALSE(args.nav_v2);
  EXPECT_FALSE(args.ai_avail_v2);
  EXPECT_FALSE(args.end_v2);
  EXPECT_FALSE(args.combat_v2);
  EXPECT_FALSE(args.orders_on);
}

// ---------------------------------------------------------------------------
// The replay listing and the screen clicks.

TEST(ReplayListTest, SortedReplayStemsOnly) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("replay_list_" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  (void)std::ofstream(dir / "b.replay");
  (void)std::ofstream(dir / "a.replay");
  (void)std::ofstream(dir / "c.json");
  const std::vector<std::string> names = render::ui::list_replays(
      dir.string());
  std::filesystem::remove_all(dir);
  EXPECT_EQ(names, (std::vector<std::string>{"a", "b"}));
}

TEST(Ui5bTest, MenuReplaysNavigatesAndBackReturns) {
  render::ui::App app =
      render::ui::sandbox_app(1280, 720, test_scenarios(), {"one"});
  app = click_at(std::move(app), center_of(app.menu, "replays"));
  EXPECT_EQ(app.screen, render::ui::Screen::Replay);
  app = click_at(std::move(app), center_of(app.replay, "back"));
  EXPECT_EQ(app.screen, render::ui::Screen::Menu);
}

TEST(Ui5bTest, EndScreenClicksAndBackToBattle) {
  render::ui::App app =
      render::ui::sandbox_app(1280, 720, test_scenarios(), {});
  app.screen = render::ui::Screen::End;
  // Back navigates; the action buttons stay in clicked for the window.
  app = click_at(std::move(app), center_of(app.end, "back"));
  EXPECT_EQ(app.screen, render::ui::Screen::Battle);

  app.screen = render::ui::Screen::End;
  app.end.clicked = render::ui::kIdRematch;
  EXPECT_TRUE(render::ui::end_clicks(app.end).rematch);
  app.end.clicked = render::ui::kIdSaveReplay;
  EXPECT_TRUE(render::ui::end_clicks(app.end).save_replay);
  app.end.clicked = render::ui::kIdSaveReport;
  EXPECT_TRUE(render::ui::end_clicks(app.end).save_report);
  app.end.clicked = "pause";  // anything else: no action
  const render::ui::EndClicks none = render::ui::end_clicks(app.end);
  EXPECT_FALSE(none.rematch);
  EXPECT_FALSE(none.save_replay);
  EXPECT_FALSE(none.save_report);
}

TEST(Ui5bTest, ReplayClicksReadLoadAndVerifyWithTheStem) {
  render::ui::App app =
      render::ui::sandbox_app(1280, 720, test_scenarios(), {"alpha", "beta"});
  app.replay.clicked = render::ui::kIdLoad;
  const render::ui::ReplayClicks load = render::ui::replay_clicks(app.replay);
  EXPECT_TRUE(load.load);
  EXPECT_FALSE(load.verify);
  EXPECT_EQ(load.load_stem, "alpha");  // selected = 0
  app.replay.clicked = render::ui::kIdVerify;
  const render::ui::ReplayClicks verify =
      render::ui::replay_clicks(app.replay);
  EXPECT_TRUE(verify.verify);
  EXPECT_TRUE(verify.load_stem.empty());
}

// ---------------------------------------------------------------------------
// The end-screen casualty curves.

TEST(EndCurvesTest, EmptySeriesDrawFrameOnly) {
  render::DrawList list;
  render::draw_end_curves(list, 0.0, 0.0, 400.0, 200.0, {}, {});
  std::size_t lines = 0;
  for (const render::DrawCmd& cmd : list.cmds) {
    if (cmd.kind == render::CmdKind::Line) {
      ++lines;
    }
  }
  EXPECT_EQ(lines, 0u);
  EXPECT_FALSE(list.cmds.empty());  // panel + frame + labels
}

TEST(EndCurvesTest, SeriesDrawLinesAndStayDeterministic) {
  const std::vector<std::uint32_t> red{10, 9, 9, 8, 7, 7, 5, 5, 4, 2, 1};
  const std::vector<std::uint32_t> blue{10, 10, 9, 9, 9, 8, 8, 7, 7, 7, 7};
  render::DrawList a;
  render::draw_end_curves(a, 0.0, 0.0, 400.0, 200.0, red, blue);
  std::size_t lines = 0;
  for (const render::DrawCmd& cmd : a.cmds) {
    if (cmd.kind == render::CmdKind::Line) {
      ++lines;
    }
  }
  EXPECT_GE(lines, 2u);  // at least one segment per faction

  render::DrawList b;
  render::draw_end_curves(b, 0.0, 0.0, 400.0, 200.0, red, blue);
  ASSERT_EQ(a.cmds.size(), b.cmds.size());
  for (std::size_t i = 0; i < a.cmds.size(); ++i) {
    EXPECT_EQ(a.cmds[i].kind, b.cmds[i].kind);
    EXPECT_EQ(a.cmds[i].text, b.cmds[i].text);
  }
}
