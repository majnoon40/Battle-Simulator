#include "sim/terrain.hpp"

#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace sim {
namespace {

[[noreturn]] void reject(const std::string& message) {
  throw std::invalid_argument(message);
}

// "#RRGGBB" -> r, g, b.
void parse_color(const std::string& text, const std::string& id,
                 std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
  const std::string message =
      "terrain type '" + id + "': invalid color '" + text +
      "' (expected \"#RRGGBB\")";
  if (text.size() != 7 || text[0] != '#') {
    reject(message);
  }
  const auto digit = [&](char c) {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    reject(message);
  };
  r = static_cast<std::uint8_t>(digit(text[1]) * 16 + digit(text[2]));
  g = static_cast<std::uint8_t>(digit(text[3]) * 16 + digit(text[4]));
  b = static_cast<std::uint8_t>(digit(text[5]) * 16 + digit(text[6]));
}

}  // namespace

TerrainGrid build_terrain_grid(std::string map_id, std::uint32_t width,
                               std::uint32_t height, double cell_size,
                               const std::vector<std::uint16_t>& cell_types,
                               const TerrainTypeTable* table) {
  if (table == nullptr) {
    reject("terrain grid needs a terrain type table");
  }
  if (width == 0 || height == 0) {
    reject("terrain map '" + map_id + "': size must be at least 1x1, got " +
           std::to_string(width) + "x" + std::to_string(height));
  }
  if (width > 4096 || height > 4096) {
    reject("terrain map '" + map_id + "': size above the 4096x4096 limit");
  }
  // m13 audit finding 11: cell_size must be finite and positive — +inf
  // passed the bare > 0 check and every world coordinate derived from
  // the grid became non-finite.
  if (!std::isfinite(cell_size) || !(cell_size > 0.0)) {
    reject("terrain map '" + map_id +
           "': cell_size must be a finite number > 0");
  }
  if (cell_types.size() != static_cast<std::size_t>(width) * height) {
    reject("terrain map '" + map_id + "': expected " +
           std::to_string(static_cast<std::size_t>(width) * height) +
           " cells for " + std::to_string(width) + "x" +
           std::to_string(height) + ", got " +
           std::to_string(cell_types.size()));
  }
  for (std::uint16_t index : cell_types) {
    if (index >= table->types.size()) {
      reject("terrain map '" + map_id + "': cell type index " +
             std::to_string(index) + " outside the table (" +
             std::to_string(table->types.size()) + " types)");
    }
  }
  TerrainGrid grid;
  grid.map_id = std::move(map_id);
  grid.width = width;
  grid.height = height;
  grid.cell_size = cell_size;
  grid.table = table;
  grid.cells = cell_types;
  return grid;
}

TerrainTypeTable parse_terrain_types(const std::string& text) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(text);
  } catch (const nlohmann::json::exception& error) {
    reject(std::string("terrain config is not valid JSON: ") + error.what());
  }
  if (!root.is_object() || !root.contains("types") ||
      !root["types"].is_array()) {
    reject("terrain config must be an object with a 'types' array");
  }
  TerrainTypeTable table;
  std::size_t line = 0;
  bool any_passable = false;
  for (const nlohmann::json& entry : root["types"]) {
    ++line;
    const std::string where =
        "terrain config, entry " + std::to_string(line);
    if (!entry.is_object()) {
      reject(where + ": expected an object");
    }
    TerrainType type;
    if (!entry.contains("id") || !entry["id"].is_string()) {
      reject(where + ": missing or non-string 'id'");
    }
    type.id = entry["id"].get<std::string>();
    if (type.id.empty()) {
      reject(where + ": 'id' must not be empty");
    }
    if (!entry.contains("color") || !entry["color"].is_string()) {
      reject(where + ": missing or non-string 'color'");
    }
    parse_color(entry["color"].get<std::string>(), type.id, type.r, type.g,
                type.b);
    if (!entry.contains("move_cost") || !entry["move_cost"].is_number()) {
      reject(where + ": missing or non-number 'move_cost'");
    }
    type.move_cost = entry["move_cost"].get<double>();
    if (!std::isfinite(type.move_cost)) {
      reject(where + ": 'move_cost' must be a finite number");
    }
    if (entry.contains("passable")) {
      if (!entry["passable"].is_boolean()) {
        reject(where + ": 'passable' must be a boolean");
      }
      type.passable = entry["passable"].get<bool>();
    }
    if (!entry.contains("sight_modifier") ||
        !entry["sight_modifier"].is_number()) {
      reject(where + ": missing or non-number 'sight_modifier'");
    }
    type.sight_modifier = entry["sight_modifier"].get<double>();
    if (!std::isfinite(type.sight_modifier)) {
      reject(where + ": 'sight_modifier' must be a finite number");
    }
    if (!entry.contains("defense_modifier") ||
        !entry["defense_modifier"].is_number()) {
      reject(where + ": missing or non-number 'defense_modifier'");
    }
    type.defense_modifier = entry["defense_modifier"].get<double>();
    if (!std::isfinite(type.defense_modifier)) {
      reject(where + ": 'defense_modifier' must be a finite number");
    }
    if (type.passable) {
      if (!(type.move_cost > 0.0)) {
        reject(where + ": passable types need move_cost > 0");
      }
      any_passable = true;
    }
    if (!(type.defense_modifier > 0.0)) {
      reject(where + ": 'defense_modifier' must be > 0");
    }
    if (type.sight_modifier < 0.0 || type.sight_modifier > 10.0) {
      reject(where + ": 'sight_modifier' must be within [0, 10]");
    }
    // Duplicate ids would make map grids ambiguous; duplicate colors would
    // make PNG pixels ambiguous.
    for (const TerrainType& other : table.types) {
      if (other.id == type.id) {
        reject(where + ": duplicate terrain id '" + type.id + "'");
      }
      if (other.r == type.r && other.g == type.g && other.b == type.b) {
        reject(where + ": color #" + entry["color"].get<std::string>() +
               " already used by terrain type '" + other.id +
               "' (map pixels must be unambiguous)");
      }
    }
    table.types.push_back(std::move(type));
  }
  if (table.types.empty()) {
    reject("terrain config: 'types' must not be empty");
  }
  if (!any_passable) {
    reject("terrain config: at least one type must be passable");
  }
  return table;
}

TerrainGrid parse_terrain_grid_json(const std::string& text, double cell_size,
                                    const TerrainTypeTable* table) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(text);
  } catch (const nlohmann::json::exception& error) {
    reject(std::string("terrain map is not valid JSON: ") + error.what());
  }
  if (!root.is_object() || !root.contains("grid") ||
      !root["grid"].is_array() || root["grid"].empty()) {
    reject("terrain map must be an object with a non-empty 'grid' array");
  }
  std::string id = "map";
  if (root.contains("id") && root["id"].is_string()) {
    id = root["id"].get<std::string>();
  }
  std::uint32_t height = 0;
  std::uint32_t width = 0;
  std::vector<std::uint16_t> cells;
  for (const nlohmann::json& row : root["grid"]) {
    if (!row.is_array() || row.empty()) {
      reject("terrain map '" + id + "': grid rows must be non-empty arrays");
    }
    if (width == 0) {
      width = static_cast<std::uint32_t>(row.size());
    } else if (row.size() != width) {
      reject("terrain map '" + id + "': ragged grid (row " +
             std::to_string(height + 1) + " has " + std::to_string(row.size()) +
             " cells, expected " + std::to_string(width) + ")");
    }
    for (const nlohmann::json& cell : row) {
      if (!cell.is_string()) {
        reject("terrain map '" + id + "': cells must be terrain type ids");
      }
      const std::string type_id = cell.get<std::string>();
      std::uint16_t index = 0;
      bool found = false;
      for (std::size_t i = 0; i < table->types.size(); ++i) {
        if (table->types[i].id == type_id) {
          index = static_cast<std::uint16_t>(i);
          found = true;
          break;
        }
      }
      if (!found) {
        reject("terrain map '" + id + "': unknown terrain type '" + type_id +
               "'");
      }
      cells.push_back(index);
    }
    ++height;
  }
  return build_terrain_grid(id, width, height, cell_size, cells, table);
}

}  // namespace sim
