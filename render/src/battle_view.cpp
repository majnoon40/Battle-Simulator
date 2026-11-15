#include "render/battle_view.hpp"

#include <algorithm>
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

  return list;
}

}  // namespace render
