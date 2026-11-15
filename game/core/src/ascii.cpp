#include "game/ascii.hpp"

#include <string>
#include <vector>

namespace game {

std::string ascii_map(const sim::Simulation& simulation, int width,
                      int height) {
  if (width < 3 || height < 3) {
    return "";
  }
  const sim::World& world = simulation.world();
  // Per-cell counts: [active red, active blue, routed red, routed blue].
  const int cells = (width) * (height);
  std::vector<std::uint32_t> active_red(cells, 0);
  std::vector<std::uint32_t> active_blue(cells, 0);
  std::vector<std::uint32_t> routed_red(cells, 0);
  std::vector<std::uint32_t> routed_blue(cells, 0);
  for (const sim::Unit& unit : world.units) {
    if (!unit.alive) {
      continue;  // dead units are not shown
    }
    const int column = static_cast<int>(
        unit.x / world.width * static_cast<double>(width));
    const int row =
        static_cast<int>(unit.y / world.height * static_cast<double>(height));
    if (column < 0 || column >= width || row < 0 || row >= height) {
      continue;
    }
    const int cell = row * width + column;
    const bool red = unit.faction == sim::Faction::Red;
    if (unit.routed) {
      (red ? routed_red : routed_blue)[cell] += 1;
    } else {
      (red ? active_red : active_blue)[cell] += 1;
    }
  }

  std::string map;
  map += "+" + std::string(width, '-') + "+\n";
  for (int row = 0; row < height; ++row) {
    map += "|";
    for (int column = 0; column < width; ++column) {
      const int cell = row * width + column;
      const std::uint32_t red =
          active_red[cell] + routed_red[cell];
      const std::uint32_t blue =
          active_blue[cell] + routed_blue[cell];
      char ch = ' ';
      if (red > 0 || blue > 0) {
        const bool mostly_red = red >= blue;
        const bool any_active =
            mostly_red ? active_red[cell] > 0 : active_blue[cell] > 0;
        if (any_active) {
          ch = mostly_red ? 'R' : 'B';
        } else {
          ch = mostly_red ? 'r' : 'b';
        }
      }
      map += ch;
    }
    map += "|\n";
  }
  map += "+" + std::string(width, '-') + "+";

  // Header and legend above the map.
  std::uint32_t alive[2] = {0, 0};
  std::uint32_t routed[2] = {0, 0};
  for (const sim::Unit& unit : world.units) {
    const std::size_t index = unit.faction == sim::Faction::Red ? 0 : 1;
    if (unit.alive) {
      ++alive[index];
      if (unit.routed) {
        ++routed[index];
      }
    }
  }
  map = "tick " + std::to_string(simulation.tick_index()) + "  seed " +
        std::to_string(simulation.config().seed) + "\n" +
        "Red " + std::to_string(alive[0]) + " alive (" +
        std::to_string(routed[0]) + " routed)  Blue " +
        std::to_string(alive[1]) + " alive (" + std::to_string(routed[1]) +
        " routed)\n" + map +
        "\nR/B active  r/b routed";
  return map;
}

}  // namespace game
