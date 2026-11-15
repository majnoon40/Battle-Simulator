#include "sim/flow_field.hpp"

#include <cmath>
#include <queue>
#include <utility>

namespace sim {

FlowField compute_flow_field(const TerrainGrid& grid, std::uint32_t target_cx,
                             std::uint32_t target_cy) {
  FlowField field;
  field.width = grid.width;
  field.height = grid.height;
  const std::size_t cells =
      static_cast<std::size_t>(grid.width) * grid.height;
  field.cost.assign(cells, FlowField::kUnreached);
  field.dir.assign(cells, FlowField::kNoDir);

  const auto cell_center = [&](std::uint32_t cx, std::uint32_t cy) {
    return std::pair<double, double>((cx + 0.5) * grid.cell_size,
                                     (cy + 0.5) * grid.cell_size);
  };

  // The target must be a passable cell.
  if (target_cx >= grid.width || target_cy >= grid.height) {
    return field;
  }
  {
    const auto [x, y] = cell_center(target_cx, target_cy);
    if (!grid.passable_at(x, y)) {
      return field;
    }
  }
  field.target_valid = true;

  // Dijkstra from the target over reversed edges: entering a cell costs
  // 1 / move_cost (time at base speed, cell size is a constant factor),
  // so roads are cheap and mountains expensive.
  using Item = std::pair<double, std::uint32_t>;  // (cost, cell index)
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
  const std::size_t target =
      static_cast<std::size_t>(target_cy) * grid.width + target_cx;
  field.cost[target] = 0.0;
  open.emplace(0.0, static_cast<std::uint32_t>(target));
  while (!open.empty()) {
    const auto [cost, index] = open.top();
    open.pop();
    if (cost > field.cost[index]) {
      continue;  // stale entry
    }
    const std::uint32_t cx = index % grid.width;
    const std::uint32_t cy = index / grid.width;
    for (int n = 0; n < 8; ++n) {
      const std::int32_t nx = static_cast<std::int32_t>(cx) +
                              kFlowNeighbors[n][0];
      const std::int32_t ny = static_cast<std::int32_t>(cy) +
                              kFlowNeighbors[n][1];
      if (nx < 0 || ny < 0 || nx >= static_cast<std::int32_t>(grid.width) ||
          ny >= static_cast<std::int32_t>(grid.height)) {
        continue;
      }
      const auto [x, y] =
          cell_center(static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny));
      const double speed = grid.speed_multiplier_at(x, y);
      if (speed <= 0.0) {
        continue;  // impassable
      }
      if (n % 2 == 1) {
        // Diagonal: no corner cutting — one orthogonal companion must be
        // passable too.
        const std::int32_t ox1 = static_cast<std::int32_t>(cx) +
                                 kFlowNeighbors[n][0];
        const std::int32_t oy1 = static_cast<std::int32_t>(cy);
        const std::int32_t ox2 = static_cast<std::int32_t>(cx);
        const std::int32_t oy2 = static_cast<std::int32_t>(cy) +
                                 kFlowNeighbors[n][1];
        const auto passable = [&](std::int32_t px, std::int32_t py) {
          if (px < 0 || py < 0 ||
              px >= static_cast<std::int32_t>(grid.width) ||
              py >= static_cast<std::int32_t>(grid.height)) {
            return false;
          }
          const auto [qx, qy] =
              cell_center(static_cast<std::uint32_t>(px),
                          static_cast<std::uint32_t>(py));
          return grid.passable_at(qx, qy);
        };
        if (!passable(ox1, oy1) && !passable(ox2, oy2)) {
          continue;
        }
      }
      double weight = 1.0 / speed;
      if (n % 2 == 1) {
        weight *= std::sqrt(2.0);
      }
      const std::size_t neighbor =
          static_cast<std::size_t>(ny) * grid.width + nx;
      const double next = cost + weight;
      if (next < field.cost[neighbor]) {
        field.cost[neighbor] = next;
        open.emplace(next, static_cast<std::uint32_t>(neighbor));
      }
    }
  }

      // Preferred step per cell: the passable neighbor with the lowest
      // cost. A neighbor's cost already includes entering that neighbor
      // (the Dijkstra edge), so the best step from here is simply the
      // cheapest neighbor — adding the enter cost again would double-count
      // slow cells and could create two-cell direction cycles. Every cell
      // has a strictly cheaper neighbor (its Dijkstra parent), so the
      // descent is acyclic. Ties break to the lowest neighbor index, so
      // the field is deterministic.
  field.reached_cells = 0;
  for (std::uint32_t cy = 0; cy < grid.height; ++cy) {
    for (std::uint32_t cx = 0; cx < grid.width; ++cx) {
      const std::size_t index = static_cast<std::size_t>(cy) * grid.width + cx;
      if (field.cost[index] >= FlowField::kUnreached) {
        continue;
      }
      ++field.reached_cells;
      if (cx == target_cx && cy == target_cy) {
        continue;  // the target itself has no direction
      }
      double best = FlowField::kUnreached;
      std::uint8_t best_dir = FlowField::kNoDir;
      for (int n = 0; n < 8; ++n) {
        const std::int32_t nx = static_cast<std::int32_t>(cx) +
                                kFlowNeighbors[n][0];
        const std::int32_t ny = static_cast<std::int32_t>(cy) +
                                kFlowNeighbors[n][1];
        if (nx < 0 || ny < 0 ||
            nx >= static_cast<std::int32_t>(grid.width) ||
            ny >= static_cast<std::int32_t>(grid.height)) {
          continue;
        }
        const std::size_t neighbor =
            static_cast<std::size_t>(ny) * grid.width + nx;
        if (field.cost[neighbor] >= FlowField::kUnreached) {
          continue;
        }
        const auto [x, y] = cell_center(static_cast<std::uint32_t>(nx),
                                        static_cast<std::uint32_t>(ny));
        if (grid.speed_multiplier_at(x, y) <= 0.0) {
          continue;
        }
        const double total = field.cost[neighbor];
        if (total < best) {
          best = total;
          best_dir = static_cast<std::uint8_t>(n);
        }
      }
      field.dir[index] = best_dir;
    }
  }
  return field;
}

bool flow_direction(const FlowField& field, std::uint32_t cx, std::uint32_t cy,
                    double& dx, double& dy) {
  if (cx >= field.width || cy >= field.height) {
    return false;
  }
  const std::size_t index = static_cast<std::size_t>(cy) * field.width + cx;
  const std::uint8_t dir = field.dir[index];
  if (dir == FlowField::kNoDir) {
    return false;
  }
  const double x = static_cast<double>(kFlowNeighbors[dir][0]);
  const double y = static_cast<double>(kFlowNeighbors[dir][1]);
  const double length = std::sqrt(x * x + y * y);
  dx = x / length;
  dy = y / length;
  return true;
}

}  // namespace sim
