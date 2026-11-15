#pragma once

#include <string>

#include "sim/terrain.hpp"

namespace cli {

// Loads a terrain map for a battle (M8). `path` is a PNG whose pixels are
// exact RGB matches of terrain type colors (an unknown color is an error),
// or a .json grid of type ids. `cell_size` comes from the scenario.
// PNG decoding lives here (never in sim/), via the pinned stb_image.
// Throws std::runtime_error with a readable message (file, pixel
// coordinates, color) for every failure.
[[nodiscard]] sim::TerrainGrid load_map(const std::string& path,
                                        double cell_size,
                                        const sim::TerrainTypeTable& table);

}  // namespace cli
