#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sim {

// ---------------------------------------------------------------------------
// Terrain (M8). One terrain type per row of data/terrain.json, one grid of
// cell type indices per map. Every terrain rule — passability, movement
// cost, sight scaling, defense scaling — lives in this module (the Terrain
//Grid query methods below); no other file may branch on a terrain type.
// ---------------------------------------------------------------------------

// One terrain type. `move_cost` is a SPEED MULTIPLIER as documented in
// data/terrain.json: a unit crossing a cell moves at speed * move_cost
// (road 1.3 = faster than grassland, forest 0.6 = slower). Impassable
// types never move anything. `sight_modifier` multiplies the sight radius
// of an observer standing in this cell's type. `defense_modifier`
// multiplies the damage taken by a unit standing in this cell's type
// (hills 0.75 = takes 75%).
struct TerrainType {
  std::string id;
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  double move_cost = 1.0;
  bool passable = true;
  double sight_modifier = 1.0;
  double defense_modifier = 1.0;
};

struct TerrainTypeTable {
  std::vector<TerrainType> types;
};

// A map: `width` x `height` cells of `cell_size` world units each, so the
// battlefield is width * cell_size by height * cell_size world units. Each
// cell holds an index into `table` (borrowed, exactly like UnitTypeTable
// in SimulationConfig). Build via build_terrain_grid(); PNG/JSON map
// decoding lives in cli/ (and game/), never here.
struct TerrainGrid {
  std::uint32_t width = 0;   // cells per row
  std::uint32_t height = 0;  // cells per column
  double cell_size = 1.0;    // world units per cell (> 0)
  std::string map_id;        // for the export, HUD and report
  const TerrainTypeTable* table = nullptr;
  std::vector<std::uint16_t> cells;  // row-major, width * height entries

  [[nodiscard]] std::uint32_t cell_count() const noexcept {
    return width * height;
  }

  // Bounds handling: world coordinates outside the battlefield map to no
  // cell. Every query below treats out-of-bounds as impassable so nothing
  // can end outside the map or inside an impassable cell.
  [[nodiscard]] bool in_bounds(double x, double y) const noexcept {
    return x >= 0.0 && y >= 0.0 && x < width * cell_size &&
           y < height * cell_size;
  }

  // The type index of the cell containing (x, y), or kNoCell when out of
  // bounds. O(1).
  static constexpr std::uint16_t kNoCell = 0xFFFF;
  [[nodiscard]] std::uint16_t type_index_at(double x,
                                             double y) const noexcept {
    if (!in_bounds(x, y)) {
      return kNoCell;
    }
    const std::uint32_t cx = static_cast<std::uint32_t>(x / cell_size);
    const std::uint32_t cy = static_cast<std::uint32_t>(y / cell_size);
    return cells[static_cast<std::size_t>(cy) * width + cx];
  }

  // --- the terrain rules (one place; see the module comment) -------------

  // True when a unit may stand in the cell containing (x, y). Out of
  // bounds is impassable.
  [[nodiscard]] bool passable_at(double x, double y) const noexcept {
    const std::uint16_t index = type_index_at(x, y);
    return index != kNoCell && table->types[index].passable;
  }

  // The speed multiplier for a step that ends at (x, y); 0 when the cell
  // is impassable or out of bounds.
  [[nodiscard]] double speed_multiplier_at(double x,
                                           double y) const noexcept {
    const std::uint16_t index = type_index_at(x, y);
    if (index == kNoCell || !table->types[index].passable) {
      return 0.0;
    }
    return table->types[index].move_cost;
  }

  // The observer's sight radius multiplier for the cell it stands in,
  // clamped to [0, max_scale] so radius queries stay valid. Out of bounds
  // keeps the unmodified radius (multiplier 1).
  [[nodiscard]] double sight_scale_at(double x, double y,
                                      double max_scale) const noexcept {
    const std::uint16_t index = type_index_at(x, y);
    if (index == kNoCell) {
      return 1.0;
    }
    const double modifier = table->types[index].sight_modifier;
    if (modifier < 0.0) {
      return 0.0;
    }
    return modifier > max_scale ? max_scale : modifier;
  }

  // The damage-taken multiplier for a unit standing at (x, y); 1 when out
  // of bounds (nothing fights there anyway).
  [[nodiscard]] double defense_factor_at(double x,
                                         double y) const noexcept {
    const std::uint16_t index = type_index_at(x, y);
    return index == kNoCell ? 1.0 : table->types[index].defense_modifier;
  }
};

// Builds a grid from decoded cell type indices (row-major). Validates
// size (> 0), cell_size (> 0), index range and cell count; throws
// std::invalid_argument with a readable message otherwise.
[[nodiscard]] TerrainGrid build_terrain_grid(
    std::string map_id, std::uint32_t width, std::uint32_t height,
    double cell_size, const std::vector<std::uint16_t>& cell_types,
    const TerrainTypeTable* table);

// Parses a terrain type table from JSON text (see data/terrain.json):
// {"types": [{"id", "color": "#RRGGBB", "move_cost", "passable",
// "sight_modifier", "defense_modifier"}, ...]}. Rejects duplicate ids,
// duplicate colors (a map pixel could not be resolved unambiguously),
// empty ids, non-positive move_cost on passable types, non-positive
// defense_modifier and out-of-range sight modifiers, and tables with no
// passable type. Throws std::invalid_argument with a readable message.
[[nodiscard]] TerrainTypeTable parse_terrain_types(const std::string& text);

// Builds a grid from a JSON map description: {"id": "...", "grid":
// [["grassland", "water", ...], ...]} — rows of terrain type ids. The
// cell size comes from the scenario, not the file. Unknown type ids and
// ragged rows are rejected with readable errors.
[[nodiscard]] TerrainGrid parse_terrain_grid_json(
    const std::string& text, double cell_size,
    const TerrainTypeTable* table);

}  // namespace sim
