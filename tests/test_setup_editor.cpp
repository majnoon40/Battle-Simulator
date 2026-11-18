// m13 item 5c — spawn anchors, the scenario writer, settings, theme
// scaling, and the setup-edit model.
#include "game/settings.hpp"
#include "game/setup_edit.hpp"
#include "render/ui/screens.hpp"
#include "render/ui/shell.hpp"
#include "render/ui/ui.hpp"
#include "sim/config.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/unit_types.hpp"

#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);

sim::UnitTypeTable squad_types() {
  return sim::parse_unit_types(R"json([
    {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0,
     "range": 2.0, "cooldown": 5, "sight_radius": 40.0},
    {"id": "archer", "health": 70.0, "damage": 9.0, "speed": 1.6,
     "range": 8.0, "cooldown": 8, "sight_radius": 60.0}
  ])json");
}

sim::SimulationConfig anchor_config(const sim::UnitTypeTable& types,
                                    std::optional<sim::SpawnAnchor> red_anchor,
                                    std::uint32_t count = 40) {
  sim::SimulationConfig config;
  config.seed = 42;
  config.max_ticks = 1;
  config.battlefield_width = 1000.0;
  config.battlefield_height = 1000.0;
  config.ai_mode = sim::AiMode::Utility;
  config.unit_types = &types;
  config.composition[kRed].push_back(
      sim::TypeCount{static_cast<std::uint32_t>(types.index_of("infantry")),
                     count});
  config.composition[kBlue].push_back(
      sim::TypeCount{static_cast<std::uint32_t>(types.index_of("infantry")),
                     count});
  config.spawn_anchors[kRed] = red_anchor;
  return config;
}

const std::vector<std::string>& test_scenarios() {
  static const std::vector<std::string> scenarios{"default", "squads"};
  return scenarios;
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
// Spawn anchors.

TEST(SpawnAnchorTest, AnchorMovesTheSpawnWindow) {
  const sim::UnitTypeTable types = squad_types();
  sim::SpawnAnchor anchor;
  anchor.x = 700.0;
  anchor.y = 200.0;
  sim::Simulation anchored(anchor_config(types, anchor));
  for (std::size_t i = 0; i < 40; ++i) {
    const sim::Unit& unit = anchored.world().units[i];
    EXPECT_GE(unit.x, 625.0) << "red unit below the anchor window";
    EXPECT_LE(unit.x, 775.0) << "red unit above the anchor window";
    EXPECT_GE(unit.y, 125.0);
    EXPECT_LE(unit.y, 275.0);
  }
  // Blue (no anchor) keeps the historical right band.
  for (std::size_t i = 40; i < 80; ++i) {
    const sim::Unit& unit = anchored.world().units[i];
    EXPECT_GE(unit.x, 850.0);
    EXPECT_LE(unit.x, 1000.0);
  }
}

TEST(SpawnAnchorTest, NoAnchorReproducesTheHistoricalSpawn) {
  const sim::UnitTypeTable types = squad_types();
  sim::SimulationConfig config = anchor_config(types, std::nullopt);
  sim::Simulation a(config);
  config.spawn_anchors[kRed] = std::nullopt;  // already null; explicit
  sim::Simulation b(config);
  for (std::size_t i = 0; i < b.world().units.size(); ++i) {
    EXPECT_DOUBLE_EQ(a.world().units[i].x, b.world().units[i].x);
    EXPECT_DOUBLE_EQ(a.world().units[i].y, b.world().units[i].y);
  }
}

TEST(SpawnAnchorTest, ScenarioParsesAndRejectsAnchors) {
  const sim::Scenario ok = sim::parse_scenario(R"json({
    "battlefield": {"width": 1000.0, "height": 1000.0},
    "spawn_band_fraction": 0.15,
    "armies": {"red": {"infantry": 10, "anchor": [100.0, 200.0]},
               "blue": {"infantry": 10}}
  })json");
  ASSERT_TRUE(ok.anchors[kRed].has_value());
  EXPECT_DOUBLE_EQ(ok.anchors[kRed]->x, 100.0);
  EXPECT_DOUBLE_EQ(ok.anchors[kRed]->y, 200.0);
  EXPECT_FALSE(ok.anchors[kBlue].has_value());
  // Off-field anchors are rejected.
  EXPECT_THROW(sim::parse_scenario(R"json({
    "battlefield": {"width": 1000.0, "height": 1000.0},
    "spawn_band_fraction": 0.15,
    "armies": {"red": {"infantry": 10, "anchor": [1200.0, 200.0]},
               "blue": {"infantry": 10}}
  })json"), std::invalid_argument);
  EXPECT_THROW(sim::parse_scenario(R"json({
    "battlefield": {"width": 1000.0, "height": 1000.0},
    "spawn_band_fraction": 0.15,
    "armies": {"red": {"infantry": 10, "anchor": [100.0]},
               "blue": {"infantry": 10}}
  })json"), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// The scenario writer.

TEST(WriteScenarioTest, RoundTripsEveryEditedField) {
  const sim::Scenario scenario = sim::parse_scenario(R"json({
    "battlefield": {"width": 800.0, "height": 600.0},
    "spawn_band_fraction": 0.2,
    "armies": {"red": {"infantry": 20, "anchor": [120.0, 300.0]},
               "blue": {"archer": 10, "anchor": [700.0, 300.0]}},
    "objectives": {"red": {"x": 700.0, "y": 300.0}},
    "squads": {"red": {"count": 2, "size": 10}},
    "spawn_pattern": "line"
  })json");
  const std::string text = sim::write_scenario(scenario);
  const sim::Scenario back = sim::parse_scenario(text);
  EXPECT_DOUBLE_EQ(back.battlefield_width, 800.0);
  EXPECT_DOUBLE_EQ(back.battlefield_height, 600.0);
  EXPECT_DOUBLE_EQ(back.spawn_band_fraction, 0.2);
  ASSERT_EQ(back.armies[kRed].size(), 1u);
  EXPECT_EQ(back.armies[kRed][0].type_id, "infantry");
  EXPECT_EQ(back.armies[kRed][0].count, 20u);
  ASSERT_TRUE(back.anchors[kRed].has_value());
  EXPECT_DOUBLE_EQ(back.anchors[kRed]->x, 120.0);
  ASSERT_EQ(back.armies[kBlue].size(), 1u);
  EXPECT_EQ(back.armies[kBlue][0].type_id, "archer");
  ASSERT_TRUE(back.anchors[kBlue].has_value());
  EXPECT_DOUBLE_EQ(back.anchors[kBlue]->x, 700.0);
  ASSERT_TRUE(back.objectives[kRed].has_value());
  EXPECT_DOUBLE_EQ(back.objectives[kRed]->y, 300.0);
  ASSERT_TRUE(back.squads[kRed].has_value());
  EXPECT_EQ(back.squads[kRed]->count, 2u);
  EXPECT_EQ(back.spawn_patterns[kRed], sim::SpawnPattern::Line);
}

TEST(WriteScenarioTest, RoundTripsShippedScenarios) {
  for (const char* name : {"default", "squads", "balanced-demo",
                           "viewer-demo"}) {
    std::ifstream file(std::string(TEST_DATA_DIR "/scenarios/") +
                        name + ".json");
    ASSERT_TRUE(file.good()) << name;
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    const sim::Scenario scenario = sim::parse_scenario(text);
    const sim::Scenario back = sim::parse_scenario(sim::write_scenario(scenario));
    ASSERT_EQ(back.armies[kRed].size(), scenario.armies[kRed].size()) << name;
    for (std::size_t i = 0; i < scenario.armies[kRed].size(); ++i) {
      EXPECT_EQ(back.armies[kRed][i].type_id, scenario.armies[kRed][i].type_id)
          << name;
      EXPECT_EQ(back.armies[kRed][i].count, scenario.armies[kRed][i].count)
          << name;
    }
    EXPECT_EQ(back.anchors[kRed].has_value(),
              scenario.anchors[kRed].has_value())
        << name;
    EXPECT_EQ(back.spawn_patterns[kBlue], scenario.spawn_patterns[kBlue])
        << name;
  }
}

// ---------------------------------------------------------------------------
// Settings, palette, theme scaling.

TEST(SettingsTest, ParseWriteRoundTripAndValidation) {
  game::Settings settings;
  settings.width = 1920;
  settings.height = 1080;
  settings.fullscreen = true;
  settings.vsync = true;
  settings.ui_scale = 1.5;
  settings.colorblind = true;
  const std::string text = game::write_settings(settings);
  game::Settings back;
  std::string error;
  ASSERT_TRUE(game::parse_settings(text, back, error)) << error;
  EXPECT_EQ(back.width, 1920);
  EXPECT_EQ(back.height, 1080);
  EXPECT_TRUE(back.fullscreen);
  EXPECT_TRUE(back.vsync);
  EXPECT_DOUBLE_EQ(back.ui_scale, 1.5);
  EXPECT_TRUE(back.colorblind);

  game::Settings out;  // unchanged on failure
  EXPECT_FALSE(game::parse_settings("{\"width\": 100}", out, error));
  EXPECT_EQ(out.width, 1280);
  EXPECT_FALSE(game::parse_settings("{\"nope\": 1}", out, error));
  EXPECT_FALSE(game::parse_settings("{\"ui_scale\": 9.0}", out, error));
}

TEST(SettingsTest, ColorblindPaletteSwapsFactionColors) {
  const game::Color normal = game::faction_color(sim::Faction::Red);
  game::set_colorblind_palette(true);
  const game::Color colorblind = game::faction_color(sim::Faction::Red);
  game::set_colorblind_palette(false);
  EXPECT_NE(normal.r, colorblind.r);
  EXPECT_NE(normal.g, colorblind.g);
  EXPECT_FALSE(game::colorblind_palette());
}

TEST(ThemeScaleTest, ScalesMetricsNotColors) {
  const render::ui::Theme base = render::ui::default_theme();
  const render::ui::Theme scaled = render::ui::scale_theme(base, 2.0);
  EXPECT_EQ(scaled.font_px, base.font_px * 2);
  EXPECT_EQ(scaled.row_height, base.row_height * 2);
  EXPECT_EQ(scaled.border_w, base.border_w);  // 1 px stays crisp
  EXPECT_EQ(scaled.panel.r, base.panel.r);
  // Out-of-range factors clamp.
  const render::ui::Theme clamped = render::ui::scale_theme(base, 9.0);
  EXPECT_EQ(clamped.font_px, base.font_px * 2);
}

// ---------------------------------------------------------------------------
// The setup-edit model.

TEST(SetupEditTest, StepsRespectMinimumAndCap) {
  const sim::UnitTypeTable types = squad_types();
  game::SetupEdit edit;
  edit.counts[0] = {10, 5};
  edit.counts[1] = {10, 5};
  // +1 works, -1 works.
  EXPECT_TRUE(game::setup_step(edit, 0, 1, +1, 20000));
  EXPECT_EQ(edit.counts[0][1], 6u);
  EXPECT_TRUE(game::setup_step(edit, 0, 1, -1, 20000));
  EXPECT_EQ(edit.counts[0][1], 5u);
  // Removing the faction's last unit is refused.
  edit.counts[1] = {1, 0};
  EXPECT_FALSE(game::setup_step(edit, 1, 0, -1, 20000));
  EXPECT_EQ(edit.counts[1][0], 1u);
  // The unit cap stops the plus step.
  edit.counts[0] = {19999, 0};
  edit.counts[1] = {1, 0};
  EXPECT_FALSE(game::setup_step(edit, 0, 0, +1, 20000));
  // Anchors clamp to the field.
  const sim::SpawnAnchor anchor = game::clamped_anchor(1200.0, -50.0,
                                                       1000.0, 800.0);
  EXPECT_DOUBLE_EQ(anchor.x, 1000.0);
  EXPECT_DOUBLE_EQ(anchor.y, 0.0);
}

TEST(SetupEditTest, ToScenarioWritesCountsAndAnchors) {
  const sim::UnitTypeTable types = squad_types();
  const sim::Scenario base = sim::parse_scenario(R"json({
    "battlefield": {"width": 1000.0, "height": 1000.0},
    "spawn_band_fraction": 0.15,
    "armies": {"red": {"infantry": 10}, "blue": {"archer": 8}},
    "squads": {"red": {"count": 1, "size": 10}}
  })json");
  const auto composition = sim::build_composition(base, types);
  game::SetupEdit edit = game::setup_from_scenario(base, types, composition);
  ASSERT_EQ(edit.counts[0].size(), 2u);
  EXPECT_EQ(edit.counts[0][0], 10u);   // infantry
  EXPECT_EQ(edit.counts[1][1], 8u);    // archer
  sim::SpawnAnchor anchor;
  anchor.x = 250.0;
  anchor.y = 250.0;
  edit.anchors[1] = anchor;
  edit.counts[0][1] = 4;  // add archers to red

  const sim::Scenario edited = game::to_scenario(base, edit, types);
  ASSERT_EQ(edited.armies[kRed].size(), 2u);  // zero counts dropped
  EXPECT_EQ(edited.armies[kRed][1].type_id, "archer");
  EXPECT_EQ(edited.armies[kRed][1].count, 4u);
  ASSERT_TRUE(edited.anchors[kBlue].has_value());
  EXPECT_DOUBLE_EQ(edited.anchors[kBlue]->y, 250.0);
  ASSERT_TRUE(edited.squads[kRed].has_value());  // passed through
  // ... and the written file parses back.
  const sim::Scenario back =
      sim::parse_scenario(sim::write_scenario(edited));
  ASSERT_TRUE(back.anchors[kBlue].has_value());
  EXPECT_DOUBLE_EQ(back.anchors[kBlue]->x, 250.0);
}

// ---------------------------------------------------------------------------
// The setup + settings screens.

TEST(Ui5cTest, MenuNavigatesToSetupAndSettingsAndBack) {
  render::ui::App app =
      render::ui::sandbox_app(1280, 720, test_scenarios(), {});
  const render::ui::Point point_1 = center_of(app.menu, "setup");
  app = click_at(std::move(app), point_1);
  EXPECT_EQ(app.screen, render::ui::Screen::Setup);
  const render::ui::Point point_2 = center_of(app.setup, "back");
  app = click_at(std::move(app), point_2);
  EXPECT_EQ(app.screen, render::ui::Screen::Menu);

  const render::ui::Point point_3 = center_of(app.menu, "settings");

  app = click_at(std::move(app), point_3);
  EXPECT_EQ(app.screen, render::ui::Screen::Settings);
  const render::ui::Point point_4 = center_of(app.settings, "back");
  app = click_at(std::move(app), point_4);
  EXPECT_EQ(app.screen, render::ui::Screen::Menu);
}

TEST(Ui5cTest, SetupStateBuildsSteppersAndClicksParse) {
  const render::ui::State setup = render::ui::setup_state(
      1280, 720, render::ui::default_theme(), {"infantry", "archer"});
  EXPECT_GE(find_id(setup, "minus-infantry"), 0);
  EXPECT_GE(find_id(setup, "count-infantry"), 0);
  EXPECT_GE(find_id(setup, "plus-archer"), 0);
  EXPECT_GE(find_id(setup, "setup-save"), 0);
  EXPECT_GE(find_id(setup, "setup-start"), 0);

  render::ui::State state = setup;
  state.clicked = "plus-archer";
  render::ui::SetupClicks clicks = render::ui::setup_clicks(state);
  EXPECT_EQ(clicks.plus_type, "archer");
  EXPECT_FALSE(clicks.save);
  state.clicked = "minus-infantry";
  clicks = render::ui::setup_clicks(state);
  EXPECT_EQ(clicks.minus_type, "infantry");
  state.clicked = "setup-save";
  clicks = render::ui::setup_clicks(state);
  EXPECT_TRUE(clicks.save);
  state.clicked = "setup-start";
  clicks = render::ui::setup_clicks(state);
  EXPECT_TRUE(clicks.start);
}

TEST(Ui5cTest, SettingsFromMapsWidgetsAndApplyClick) {
  render::ui::App app =
      render::ui::sandbox_app(1280, 720, test_scenarios(), {});
  // Defaults: 1280x720, everything off, slider 0.2 -> 1.0x.
  const game::Settings defaults = render::ui::settings_from(app.settings);
  EXPECT_EQ(defaults.width, 1280);
  EXPECT_EQ(defaults.height, 720);
  EXPECT_FALSE(defaults.fullscreen);
  EXPECT_DOUBLE_EQ(defaults.ui_scale, 1.0);
  EXPECT_FALSE(render::ui::settings_apply_clicked(app.settings));

  // Flip widgets by hand (the reducer path is covered by the ui tests).
  for (render::ui::Widget& widget : app.settings.tree.widgets) {
    if (widget.id == render::ui::kIdResolution) {
      widget.selected = 2;
    } else if (widget.id == render::ui::kIdFullscreen) {
      widget.on = true;
    } else if (widget.id == render::ui::kIdUiScale) {
      widget.value = 1.0;  // -> 2.0x
    }
  }
  const game::Settings changed = render::ui::settings_from(app.settings);
  EXPECT_EQ(changed.width, 1920);
  EXPECT_EQ(changed.height, 1080);
  EXPECT_TRUE(changed.fullscreen);
  EXPECT_DOUBLE_EQ(changed.ui_scale, 2.0);
  app.settings.clicked = render::ui::kIdSettingsApply;
  EXPECT_TRUE(render::ui::settings_apply_clicked(app.settings));
}
