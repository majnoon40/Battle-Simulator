#include "game/setup_edit.hpp"

#include <algorithm>

#include "sim/config.hpp"

namespace game {

SetupEdit setup_from_scenario(
    const sim::Scenario& scenario, const sim::UnitTypeTable& types,
    const std::array<std::vector<sim::TypeCount>, 2>& composition) {
  SetupEdit edit;
  for (std::size_t faction = 0; faction < 2; ++faction) {
    edit.counts[faction].assign(types.types.size(), 0);
    for (const sim::TypeCount& entry : composition[faction]) {
      if (entry.type_index < edit.counts[faction].size()) {
        edit.counts[faction][entry.type_index] = entry.count;
      }
    }
    edit.anchors[faction] = scenario.anchors[faction];
  }
  return edit;
}

bool setup_step(SetupEdit& edit, std::size_t faction, std::size_t type_index,
                int delta, std::size_t unit_cap) {
  if (faction >= 2 || type_index >= edit.counts[faction].size()) {
    return false;
  }
  std::uint32_t& count = edit.counts[faction][type_index];
  if (delta < 0) {
    if (count == 0) {
      return false;
    }
    // Removing the faction's last unit is refused (an empty army is not
    // a valid scenario).
    std::uint64_t total = 0;
    for (std::uint32_t value : edit.counts[faction]) {
      total += value;
    }
    if (total == 1 && count == 1) {
      return false;
    }
    --count;
    return true;
  }
  if (delta > 0) {
    std::uint64_t total = 0;
    for (const auto& per_faction : edit.counts) {
      for (std::uint32_t value : per_faction) {
        total += value;
      }
    }
    if (total >= unit_cap) {
      return false;
    }
    ++count;
    return true;
  }
  return false;
}

sim::SpawnAnchor clamped_anchor(double x, double y, double world_w,
                                double world_h) {
  sim::SpawnAnchor anchor;
  anchor.x = std::clamp(x, 0.0, world_w);
  anchor.y = std::clamp(y, 0.0, world_h);
  return anchor;
}

sim::Scenario to_scenario(const sim::Scenario& base, const SetupEdit& edit,
                          const sim::UnitTypeTable& types) {
  sim::Scenario out = base;
  for (std::size_t faction = 0; faction < 2; ++faction) {
    out.armies[faction].clear();
    out.budgets[faction].reset();
    for (std::size_t type_index = 0;
         type_index < edit.counts[faction].size() && type_index < types.size();
         ++type_index) {
      if (edit.counts[faction][type_index] > 0) {
        sim::ScenarioEntry entry;
        entry.type_id = types.types[type_index].id;
        entry.count = edit.counts[faction][type_index];
        out.armies[faction].push_back(entry);
      }
    }
    out.anchors[faction] = edit.anchors[faction];
  }
  return out;
}

}  // namespace game
