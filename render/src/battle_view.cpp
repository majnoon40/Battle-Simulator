#include "game/battle_input.hpp"
#include "render/battle_view.hpp"

#include <algorithm>
#include <utility>
#include <cmath>
#include <string>
#include <vector>

#include "game/theme.hpp"
#include "sim/factions.hpp"
#include "sim/squads.hpp"

namespace render {

void DrawList::circle(double x, double y, double radius, game::Color fill,
                      game::Color outline) {
  DrawCmd cmd;
  cmd.kind = CmdKind::Circle;
  cmd.a = x;
  cmd.b = y;
  cmd.c = radius;
  cmd.color = fill;
  if (outline.a != 0) {
    cmd.d = 1.0;  // also draw a 1px outline in color2
    cmd.color2 = outline;
  }
  cmds.push_back(std::move(cmd));
}

void DrawList::triangle(double x1, double y1, double x2, double y2, double x3,
                        double y3, game::Color fill) {
  DrawCmd cmd;
  cmd.kind = CmdKind::Triangle;
  cmd.a = x1;
  cmd.b = y1;
  cmd.c = x2;
  cmd.d = y2;
  cmd.e = x3;
  cmd.f = y3;
  cmd.color = fill;
  cmds.push_back(std::move(cmd));
}

void DrawList::line(double x1, double y1, double x2, double y2,
                    double thickness, game::Color color) {
  DrawCmd cmd;
  cmd.kind = CmdKind::Line;
  cmd.a = x1;
  cmd.b = y1;
  cmd.c = x2;
  cmd.d = y2;
  cmd.e = thickness;
  cmd.color = color;
  cmds.push_back(std::move(cmd));
}

void DrawList::rect(double x, double y, double w, double h, game::Color color,
                    bool filled) {
  DrawCmd cmd;
  cmd.kind = CmdKind::Rect;
  cmd.a = x;
  cmd.b = y;
  cmd.c = w;
  cmd.d = h;
  cmd.e = filled ? 1.0 : 0.0;
  cmd.color = color;
  cmds.push_back(std::move(cmd));
}

void DrawList::text(double x, double y, int font_px, const std::string& content,
                    game::Color color) {
  DrawCmd cmd;
  cmd.kind = CmdKind::Text;
  cmd.a = x;
  cmd.b = y;
  cmd.c = static_cast<double>(font_px);
  cmd.color = color;
  cmd.text = content;
  cmds.push_back(std::move(cmd));
}

namespace {

// The unit type id, or a stable placeholder for legacy battles (no type
// table): every such unit draws as the fallback circle.
std::string type_id_of(const sim::Simulation& simulation,
                       const sim::Unit& unit) {
  if (simulation.config().unit_types != nullptr) {
    return simulation.config().unit_types->types[unit.type_index].id;
  }
  return "unit";
}

// A translucent panel behind a block of text lines (rounded look is not
// needed; just readable contrast).
void panel(DrawList& list, double x, double y, double w, double h) {
  list.rect(x, y, w, h, game::panel_color(), true);
}

void text_block(DrawList& list, const std::vector<std::string>& lines,
                double x, double y, int font_px, double line_height) {
  for (const std::string& line : lines) {
    list.text(x, y, font_px, line, game::text_color());
    y += line_height;
  }
}

}  // namespace

DrawList build_battle_draw_list(const sim::Simulation& simulation,
                                const game::Camera& camera,
                                const game::ViewOptions& options,
                                int screen_width, int screen_height,
                                const HudStats& stats) {
  DrawList list;
  list.width = screen_width;
  list.height = screen_height;
  list.background = game::background_color();
  const sim::World& world = simulation.world();

  // Terrain layer (M8): run-length merged cell rows in the terrain
  // palette, the bottom-most layer. Large maps collapse to a few hundred
  // rects instead of one per cell, so both backends stay fast.
  if (options.show_terrain && simulation.terrain() != nullptr) {
    const sim::TerrainGrid& terrain = *simulation.terrain();
    const double cell = terrain.cell_size;
    const std::uint32_t cx_begin = static_cast<std::uint32_t>(
        std::clamp(camera.world_x(0.0, screen_width) / cell, 0.0,
                   static_cast<double>(terrain.width)));
    const std::uint32_t cx_end = static_cast<std::uint32_t>(
        std::clamp(camera.world_x(static_cast<double>(screen_width),
                                  screen_width) /
                       cell + 1.0,
                   0.0, static_cast<double>(terrain.width)));
    const std::uint32_t cy_begin = static_cast<std::uint32_t>(
        std::clamp(camera.world_y(0.0, screen_height) / cell, 0.0,
                   static_cast<double>(terrain.height)));
    const std::uint32_t cy_end = static_cast<std::uint32_t>(
        std::clamp(camera.world_y(static_cast<double>(screen_height),
                                  screen_height) /
                       cell + 1.0,
                   0.0, static_cast<double>(terrain.height)));
    for (std::uint32_t cy = cy_begin; cy < cy_end; ++cy) {
      const double sy = camera.screen_y(cy * cell, screen_height);
      const double sh = cell * camera.zoom;
      std::uint32_t cx = cx_begin;
      while (cx < cx_end) {
        const std::uint16_t type =
            terrain.cells[static_cast<std::size_t>(cy) * terrain.width + cx];
        std::uint32_t run_end = cx + 1;
        while (run_end < cx_end &&
               terrain.cells[static_cast<std::size_t>(cy) * terrain.width +
                             run_end] == type) {
          ++run_end;
        }
        const sim::TerrainType& shape = terrain.table->types[type];
        const game::Color color{shape.r, shape.g, shape.b, 255};
        list.rect(camera.screen_x(cx * cell, screen_width), sy,
                  static_cast<double>(run_end - cx) * cell * camera.zoom, sh,
                  color, true);
        cx = run_end;
      }
    }
  }

  // World grid: a line every 100 world units inside the visible range.
  if (options.show_grid) {
    const double left = camera.world_x(0.0, screen_width);
    const double right =
        camera.world_x(static_cast<double>(screen_width), screen_width);
    const double top = camera.world_y(0.0, screen_height);
    const double bottom =
        camera.world_y(static_cast<double>(screen_height), screen_height);
    for (double gx = std::ceil(left / 100.0) * 100.0; gx <= right;
         gx += 100.0) {
      if (gx < 0.0 || gx > world.width) {
        continue;
      }
      const double sx = camera.screen_x(gx, screen_width);
      list.line(sx, 0.0, sx, static_cast<double>(screen_height), 1.0,
                game::grid_color());
    }
    for (double gy = std::ceil(top / 100.0) * 100.0; gy <= bottom;
         gy += 100.0) {
      if (gy < 0.0 || gy > world.height) {
        continue;
      }
      const double sy = camera.screen_y(gy, screen_height);
      list.line(0.0, sy, static_cast<double>(screen_width), sy, 1.0,
                game::grid_color());
    }
  }

  const bool squads = simulation.squads_active();

  // Formation slots: every member's slot position as a small translucent
  // square (the slot the member steers to when it holds formation).
  if (options.formation_slots && squads) {
    for (const sim::Squad& squad : simulation.squads()) {
      const game::Color slot_color = game::faction_color(squad.faction);
      game::Color translucent = slot_color;
      translucent.a = 110;
      for (std::size_t m = 0; m < squad.members.size(); ++m) {
        double wx = 0.0;
        double wy = 0.0;
        if (!simulation.squad_slot_position(static_cast<std::int32_t>(squad.id),
                                            m, wx, wy)) {
          continue;
        }
        const double sx = camera.screen_x(wx, screen_width);
        const double sy = camera.screen_y(wy, screen_height);
        if (sx < -4.0 || sy < -4.0 || sx > screen_width + 4.0 ||
            sy > screen_height + 4.0) {
          continue;
        }
        list.rect(sx - 1.0, sy - 1.0, 2.0, 2.0, translucent, true);
      }
    }
  }

  // Squad links: a thin line from every alive member to its commander (or
  // the squad centroid when leaderless). The commander itself links to
  // nothing.
  if (options.squad_links && squads) {
    for (const sim::Squad& squad : simulation.squads()) {
      double anchor_x = squad.centroid_x;
      double anchor_y = squad.centroid_y;
      if (squad.command_active) {
        const sim::Unit& commander = world.units[squad.commander_index];
        anchor_x = commander.x;
        anchor_y = commander.y;
      }
      const double anchor_sx = camera.screen_x(anchor_x, screen_width);
      const double anchor_sy = camera.screen_y(anchor_y, screen_height);
      for (const sim::SquadMemberState& member : squad.members) {
        if (member.unit_index == squad.commander_index) {
          continue;
        }
        const sim::Unit& unit = world.units[member.unit_index];
        if (!unit.alive) {
          continue;
        }
        const double sx = camera.screen_x(unit.x, screen_width);
        const double sy = camera.screen_y(unit.y, screen_height);
        list.line(sx, sy, anchor_sx, anchor_sy, 1.0, game::link_color());
      }
    }
  }

  // Sight radius of the selected unit: an outline circle.
  if (options.show_sight_radius && options.selected_unit >= 0 &&
      static_cast<std::size_t>(options.selected_unit) < world.units.size()) {
    const sim::Unit& unit = world.units[options.selected_unit];
    if (unit.alive) {
      game::Color outline = game::faction_color(unit.faction);
      outline.a = 160;
      DrawCmd cmd;
      cmd.kind = CmdKind::Circle;
      cmd.a = camera.screen_x(unit.x, screen_width);
      cmd.b = camera.screen_y(unit.y, screen_height);
      cmd.c = unit.stats.sight_radius * camera.zoom;
      cmd.e = 1.0;  // outline only, no fill
      cmd.color2 = outline;
      list.cmds.push_back(std::move(cmd));
    }
  }

  // Units. Dead units are not drawn; routed units are desaturated; the
  // selected unit gets a white ring; commanders get a gold outline. Below
  // the point-zoom threshold every unit is a single pixel so 20,000-unit
  // battles stay readable at full view.
  const bool points = camera.zoom < options.point_zoom_threshold;
  for (std::size_t i = 0; i < world.units.size(); ++i) {
    const sim::Unit& unit = world.units[i];
    if (!unit.alive) {
      continue;
    }
    const double sx = camera.screen_x(unit.x, screen_width);
    const double sy = camera.screen_y(unit.y, screen_height);
    const game::Color color =
        unit.routed ? game::routed_color(unit.faction)
                    : game::faction_color(unit.faction);
    const std::string type_id = type_id_of(simulation, unit);
    const double margin = game::unit_size(type_id) * camera.zoom + 4.0;
    if (sx < -margin || sy < -margin || sx > screen_width + margin ||
        sy > screen_height + margin) {
      continue;  // off-screen
    }
    if (points) {
      list.rect(sx, sy, 1.0, 1.0, color, true);
      continue;
    }
    const double size = game::unit_size(type_id) * camera.zoom;
    switch (game::shape_of(type_id)) {
      case game::UnitShape::Circle:
        list.circle(sx, sy, size, color,
                    game::is_commander_type(unit)
                        ? game::commander_outline()
                        : game::Color{});
        break;
      case game::UnitShape::Triangle:
        list.triangle(sx, sy - size, sx - 0.87 * size, sy + 0.5 * size,
                      sx + 0.87 * size, sy + 0.5 * size, color);
        break;
      case game::UnitShape::Rect:
        list.rect(sx - size, sy - size, 2.0 * size, 2.0 * size, color, true);
        break;
    }
    if (static_cast<std::int32_t>(i) == options.selected_unit) {
      DrawCmd ring;
      ring.kind = CmdKind::Circle;
      ring.a = sx;
      ring.b = sy;
      ring.c = size + 2.0;
      ring.e = 1.0;  // outline only
      ring.color2 = game::selected_outline();
      list.cmds.push_back(std::move(ring));
    }
  }

  // Squad badges at the centroids: a small panel with the squad id, the
  // clickable target for the squad panel. Selected squad highlighted.
  if (options.show_badges && squads) {
    for (const sim::Squad& squad : simulation.squads()) {
      const double sx = camera.screen_x(squad.centroid_x, screen_width);
      const double sy = camera.screen_y(squad.centroid_y, screen_height);
      if (sx < -20.0 || sy < -20.0 || sx > screen_width + 20.0 ||
          sy > screen_height + 20.0) {
        continue;
      }
      const bool selected =
          static_cast<std::int32_t>(squad.id) == options.selected_squad;
      game::Color badge = game::panel_color();
      if (selected) {
        badge = game::faction_color(squad.faction);
      }
      const std::string label = std::to_string(squad.id);
      // 6px per glyph at font 7, plus 4px padding.
      const double width = 8.0 + 6.0 * static_cast<double>(label.size());
      list.rect(sx - width / 2.0, sy - 7.0, width, 14.0, badge, true);
      list.text(sx - width / 2.0 + 4.0, sy - 3.0, 7, label,
                game::text_color());
    }
  }

  // m13 item 5a: multi-squad selection — an outline ring on every alive
  // member of each selected squad (a 2 px mark in point mode), in the
  // same accent the selected unit uses.
  if (!options.selected_squads.empty() && squads) {
    const bool points_mode = camera.zoom < options.point_zoom_threshold;
    for (const sim::Squad& squad : simulation.squads()) {
      if (std::find(options.selected_squads.begin(),
                    options.selected_squads.end(),
                    static_cast<std::int32_t>(squad.id)) ==
          options.selected_squads.end()) {
        continue;
      }
      for (const sim::SquadMemberState& member : squad.members) {
        const sim::Unit& unit = world.units[member.unit_index];
        if (!unit.alive) {
          continue;
        }
        const double sx = camera.screen_x(unit.x, screen_width);
        const double sy = camera.screen_y(unit.y, screen_height);
        if (sx < -4.0 || sy < -4.0 || sx > screen_width + 4.0 ||
            sy > screen_height + 4.0) {
          continue;
        }
        if (points_mode) {
          list.rect(sx - 1.0, sy - 1.0, 2.0, 2.0, game::selected_outline(),
                    true);
          continue;
        }
        DrawCmd ring;
        ring.kind = CmdKind::Circle;
        ring.a = sx;
        ring.b = sy;
        ring.c = 3.0;
        ring.e = 1.0;  // outline only
        ring.color2 = game::selected_outline();
        list.cmds.push_back(std::move(ring));
      }
    }
  }

  // m13 item 5a: order markers. Point orders draw a line from the squad
  // centroid to the target, a small circle there and the order glyph;
  // point-less orders (Hold/Retreat/Regroup/formation) draw the glyph
  // above the squad badge.
  if (options.show_order_markers && squads) {
    for (const sim::Squad& squad : simulation.squads()) {
      if (!squad.player_order_active) {
        continue;
      }
      char glyph = '?';
      switch (squad.player_order_type) {
        case sim::PlayerOrderType::MoveTo:
          glyph = 'M';
          break;
        case sim::PlayerOrderType::AttackMove:
          glyph = 'A';
          break;
        case sim::PlayerOrderType::Hold:
          glyph = 'H';
          break;
        case sim::PlayerOrderType::Retreat:
          glyph = 'R';
          break;
        case sim::PlayerOrderType::Regroup:
          glyph = 'G';
          break;
        case sim::PlayerOrderType::ChangeFormation:
          glyph = 'F';
          break;
      }
      const double csx =
          camera.screen_x(squad.centroid_x, screen_width);
      const double csy =
          camera.screen_y(squad.centroid_y, screen_height);
      game::Color accent = game::faction_color(squad.faction);
      accent.a = 220;
      if (squad.player_order_point) {
        const double tsx =
            camera.screen_x(squad.player_order_x, screen_width);
        const double tsy =
            camera.screen_y(squad.player_order_y, screen_height);
        list.line(csx, csy, tsx, tsy, 1.0, accent);
        list.circle(tsx, tsy, 3.0, game::Color{}, accent);
        list.text(tsx + 5.0, tsy - 3.0, 7, std::string(1, glyph),
                  game::text_color());
      } else {
        list.text(csx - 2.0, csy - 22.0, 7, std::string(1, glyph),
                  game::text_color());
      }
    }
  }

  // HUD (top-left).
  if (options.show_hud) {
    const std::vector<std::string> lines =
        game::hud_lines(simulation, stats.fps, stats.sim_tps, stats.speed,
                        stats.limited, stats.scenario_name);
    const double block_height =
        12.0 + 18.0 * static_cast<double>(lines.size());
    panel(list, 0.0, 0.0, 320.0, block_height);
    text_block(list, lines, 8.0, 6.0, 14, 18.0);
  }

  // Inspector (top-right).
  if (options.selected_unit >= 0) {
    const std::vector<std::string> lines =
        game::inspector_lines(simulation,
                              static_cast<std::size_t>(options.selected_unit));
    if (!lines.empty()) {
      const double block_height =
          12.0 + 18.0 * static_cast<double>(lines.size());
      panel(list, static_cast<double>(screen_width) - 330.0, 0.0, 330.0,
            block_height);
      text_block(list, lines, static_cast<double>(screen_width) - 322.0, 6.0,
                 14, 18.0);
    }
  }

  // Squad panel (bottom-right).
  if (options.selected_squad >= 0) {
    const std::vector<std::string> lines =
        game::squad_panel_lines(simulation, options.selected_squad);
    if (!lines.empty()) {
      const double block_height =
          12.0 + 18.0 * static_cast<double>(lines.size());
      panel(list, static_cast<double>(screen_width) - 330.0,
            static_cast<double>(screen_height) - block_height, 330.0,
            block_height);
      text_block(list, lines, static_cast<double>(screen_width) - 322.0,
                 static_cast<double>(screen_height) - block_height + 6.0, 14,
                 18.0);
    }
  }

  // m13 item 5a: the minimap — bottom-left square: panel background,
  // a coarse terrain thumbnail (4-px blocks) when a map is loaded, unit
  // dots, and the camera viewport outline. Click-to-jump is the window's
  // job (game::world_x/y_from_minimap); this is only the drawing.
  if (options.show_minimap) {
    const game::MinimapGeometry geometry = game::minimap_geometry(
        screen_width, screen_height, world.width, world.height);
    list.rect(static_cast<double>(geometry.x),
              static_cast<double>(geometry.y),
              static_cast<double>(geometry.size),
              static_cast<double>(geometry.size), game::panel_color(),
              true);
    if (const sim::TerrainGrid* terrain = simulation.terrain()) {
      const int block = 4;  // minimap pixels per terrain sample
      for (int by = 0; by + block <= geometry.size; by += block) {
        for (int bx = 0; bx + block <= geometry.size; bx += block) {
          const double wx = game::world_x_from_minimap(
              geometry,
              static_cast<double>(geometry.x + bx + block / 2.0));
          const double wy = game::world_y_from_minimap(
              geometry,
              static_cast<double>(geometry.y + by + block / 2.0));
          const std::uint16_t type = terrain->type_index_at(wx, wy);
          if (type == sim::TerrainGrid::kNoCell) {
            continue;
          }
          const sim::TerrainType& shape = terrain->table->types[type];
          list.rect(static_cast<double>(geometry.x + bx),
                    static_cast<double>(geometry.y + by),
                    static_cast<double>(block),
                    static_cast<double>(block),
                    game::Color{shape.r, shape.g, shape.b, 255}, true);
        }
      }
    }
    for (const sim::Unit& unit : world.units) {
      if (!unit.alive) {
        continue;
      }
      const double mx = game::minimap_x(geometry, unit.x);
      const double my = game::minimap_y(geometry, unit.y);
      const game::Color color =
          unit.routed ? game::routed_color(unit.faction)
                      : game::faction_color(unit.faction);
      list.rect(mx - 1.0, my - 1.0, 2.0, 2.0, color, true);
    }
    // Camera viewport outline.
    {
      const double vx0 = game::minimap_x(
          geometry, camera.world_x(0.0, screen_width));
      const double vy0 = game::minimap_y(
          geometry, camera.world_y(0.0, screen_height));
      const double vx1 = game::minimap_x(
          geometry,
          camera.world_x(static_cast<double>(screen_width),
                         screen_width));
      const double vy1 = game::minimap_y(
          geometry,
          camera.world_y(static_cast<double>(screen_height),
                         screen_height));
      DrawCmd frame;
      frame.kind = CmdKind::Rect;
      frame.a = vx0;
      frame.b = vy0;
      frame.c = vx1 - vx0;
      frame.d = vy1 - vy0;
      frame.e = 0.0;  // outline
      frame.color = game::Color{255, 255, 255, 180};
      list.cmds.push_back(std::move(frame));
    }
    DrawCmd border;
    border.kind = CmdKind::Rect;
    border.a = static_cast<double>(geometry.x);
    border.b = static_cast<double>(geometry.y);
    border.c = static_cast<double>(geometry.size);
    border.d = static_cast<double>(geometry.size);
    border.e = 0.0;  // outline
    border.color = game::text_color();
    list.cmds.push_back(std::move(border));
  }

  return list;
}

void draw_end_curves(DrawList& list, double x, double y, double w,
                     double h, const std::vector<std::uint32_t>& red,
                     const std::vector<std::uint32_t>& blue) {
  list.rect(x, y, w, h, game::panel_color(), true);
  list.rect(x, y, w, h, game::text_color(), false);
  list.text(x + 8.0, y + 6.0, 7, "alive per faction over ticks",
            game::text_color());
  list.rect(x + 8.0, y + 20.0, 6.0, 6.0,
            game::faction_color(sim::Faction::Red), true);
  list.text(x + 18.0, y + 18.0, 7, "red", game::text_color());
  list.rect(x + 48.0, y + 20.0, 6.0, 6.0,
            game::faction_color(sim::Faction::Blue), true);
  list.text(x + 58.0, y + 18.0, 7, "blue", game::text_color());

  if (red.empty() && blue.empty()) {
    return;
  }
  const std::size_t ticks =
      std::max(red.size(), blue.size());
  std::uint32_t peak = 1;
  for (const std::uint32_t value : red) {
    peak = std::max(peak, value);
  }
  for (const std::uint32_t value : blue) {
    peak = std::max(peak, value);
  }
  // The plot area sits under the title/legend strip.
  const double plot_x = x + 8.0;
  const double plot_y = y + 32.0;
  const double plot_w = w - 16.0;
  const double plot_h = h - 40.0;
  const auto point_of = [&](std::size_t tick, std::uint32_t value) {
    const double fx = ticks <= 1
                          ? 0.0
                          : static_cast<double>(tick) /
                                static_cast<double>(ticks - 1);
    const double fy = static_cast<double>(value) /
                      static_cast<double>(peak);
    return std::pair<double, double>{
        plot_x + fx * plot_w, plot_y + (1.0 - fy) * plot_h};
  };
  const auto draw_series = [&](const std::vector<std::uint32_t>& series,
                               game::Color color) {
    if (series.empty()) {
      return;
    }
    const std::size_t step = std::max<std::size_t>(1, ticks / 120);
    std::size_t previous = 0;
    for (std::size_t tick = step; tick < series.size(); tick += step) {
      const auto [x0, y0] = point_of(previous, series[previous]);
      const auto [x1, y1] = point_of(tick, series[tick]);
      list.line(x0, y0, x1, y1, 2.0, color);
      previous = tick;
    }
    const auto [x0, y0] = point_of(previous, series[previous]);
    const auto [x1, y1] = point_of(series.size() - 1, series.back());
    list.line(x0, y0, x1, y1, 2.0, color);
  };
  draw_series(red, game::faction_color(sim::Faction::Red));
  draw_series(blue, game::faction_color(sim::Faction::Blue));
}

}  // namespace render
