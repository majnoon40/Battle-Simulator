#pragma once

#include <vector>

#include "sim/unit.hpp"

namespace sim {

// Index of the nearest living enemy of `unit` within `units`, or -1 when no
// living enemy exists. Distances are compared squared. Ties are broken by the
// lower unit id, so the result does not depend on the order of `units`.
//
// M1 uses this O(n) scan directly; the spatial hash required for large
// battles arrives in a later milestone.
[[nodiscard]] int nearest_enemy_index(const std::vector<Unit>& units,
                                      const Unit& unit);

}  // namespace sim
