#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sim/factions.hpp"
#include "sim/simulation.hpp"

namespace game {

// One RGBA color, 8 bits per channel. Plain data so DrawList commands and
// the rasterizer stay byte-deterministic.
struct Color {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 255;

  [[nodiscard]] constexpr bool operator==(const Color&) const = default;
};

// The shapes a unit type can draw as (mapped by type id, see shape_of).
enum class UnitShape : std::uint8_t { Circle, Triangle, Rect };

// Unit visual size in world units, by type id (fallback: infantry-sized).
[[nodiscard]] double unit_size(const std::string& type_id);

// The shape a unit type draws as: infantry -> circle, archer -> triangle,
// cavalry -> rect, commander -> rect; anything else -> circle (fallback).
[[nodiscard]] UnitShape shape_of(const std::string& type_id);

// Faction colors (vivid for active units).
[[nodiscard]] Color faction_color(sim::Faction faction);
// The same hue desaturated (mixed toward its luminance) for routed units.
[[nodiscard]] Color routed_color(sim::Faction faction);
// The commander outline and the selection highlight.
[[nodiscard]] constexpr Color commander_outline() {
  return Color{255, 200, 60, 255};
}
[[nodiscard]] constexpr Color selected_outline() {
  return Color{255, 255, 255, 255};
}
// Viewer chrome: background, grid, squad links, text.
[[nodiscard]] constexpr Color background_color() {
  return Color{24, 26, 32, 255};
}
[[nodiscard]] constexpr Color grid_color() { return Color{44, 48, 60, 255}; }
[[nodiscard]] constexpr Color link_color() { return Color{140, 140, 160, 70}; }
[[nodiscard]] constexpr Color text_color() { return Color{230, 232, 240, 255}; }
[[nodiscard]] constexpr Color panel_color() { return Color{16, 18, 24, 200}; }

// True when the unit type is a commander type (draws an outline).
[[nodiscard]] bool is_commander_type(const sim::Unit& unit);

// HUD lines (top-left block): tick, fps and sim throughput, speed and the
// "sim limited" flag, seed, per-faction alive/routed/dead. `fps` and
// `sim_tps` are measured by the caller (<= 0 prints a placeholder).
[[nodiscard]] std::vector<std::string> hud_lines(
    const sim::Simulation& simulation, double fps, double sim_tps, int speed,
    bool limited, const std::string& scenario_name);

// Inspector lines for one unit (health, morale and state, personality,
// squad, current action, order) and its utility scores when the AI debug
// capture is armed on exactly this unit.
[[nodiscard]] std::vector<std::string> inspector_lines(
    const sim::Simulation& simulation, std::size_t unit_index);

// Squad panel lines for one squad id (faction, formation, members,
// commander, order, cohesion, cumulative squad statistics).
[[nodiscard]] std::vector<std::string> squad_panel_lines(
    const sim::Simulation& simulation, std::int32_t squad_id);

}  // namespace game
