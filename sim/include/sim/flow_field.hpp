#pragma once

#include <cstdint>
#include <vector>

#include "sim/terrain.hpp"

namespace sim {

// ---------------------------------------------------------------------------
// Flow field navigation (M8). One cached field per faction, computed at
// battle start (and recomputed if the faction objective ever moves to a
// different cell): a Dijkstra cost field over the terrain grid from the
// objective cell, where crossing a cell costs 1 / move_cost (the time a
// unit at base speed needs, up to a constant), plus one preferred-step
// direction per cell. Impassable cells are never entered; a diagonal step
// is only allowed when at least one of its orthogonal neighbors is
// passable (no corner cutting through walls).
// ---------------------------------------------------------------------------

struct FlowField {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // Cost from each cell TO the target (the Dijkstra tree is rooted at the
  // target; a unit descends the cost). kUnreached when no path exists.
  std::vector<double> cost;
  // Preferred step per cell: an index into kFlowNeighbors (below), or
  // kNoDir for the target cell itself and unreached cells.
  std::vector<std::uint8_t> dir;
  std::uint32_t reached_cells = 0;  // passable cells with a finite cost
  bool target_valid = false;        // false when the target is impassable

  static constexpr double kUnreached = 1e30;
  static constexpr std::uint8_t kNoDir = 8;
};

// The 8 neighbor steps in fixed order (ties in direction selection break
// to the first of these). Screen/world y grows downward, like the grid.
inline constexpr std::int8_t kFlowNeighbors[8][2] = {
    {1, 0},   // 0 E
    {1, -1},  // 1 NE
    {0, -1},  // 2 N
    {-1, -1}, // 3 NW
    {-1, 0},  // 4 W
    {-1, 1},  // 5 SW
    {0, 1},   // 6 S
    {1, 1},   // 7 SE
};

// Computes the field from the target cell. Deterministic: the cost field
// is unique, and direction ties break to the lowest kFlowNeighbors index.
[[nodiscard]] FlowField compute_flow_field(const TerrainGrid& grid,
                                           std::uint32_t target_cx,
                                           std::uint32_t target_cy);

// The unit-length step direction a unit in cell (cx, cy) should take to
// descend the field; false when the cell has no direction (unreached,
// impassable or the target itself). `dx`/`dy` form a unit vector (0 for a
// straight step, +/- 1/sqrt(2) for diagonals).
[[nodiscard]] bool flow_direction(const FlowField& field, std::uint32_t cx,
                                  std::uint32_t cy, double& dx, double& dy);

}  // namespace sim
