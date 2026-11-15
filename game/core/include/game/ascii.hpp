#pragma once

#include <string>

#include "sim/simulation.hpp"

namespace game {

// A compact terminal map of the battle (M7 --ascii): a `width` x `height`
// character grid over the battlefield, one row of border, then the header
// and a legend. Cell characters: 'R'/'B' = majority faction active units,
// 'r'/'b' = majority routed, '.' = only dead/absent covered... precisely:
// actives win over routed, routed win over nothing; a cell with any alive
// units prints the majority faction (upper for active, lower for routed);
// an empty cell prints ' '. Dead units are not shown.
[[nodiscard]] std::string ascii_map(const sim::Simulation& simulation,
                                    int width = 78, int height = 24);

}  // namespace game
