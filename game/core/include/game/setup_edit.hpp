#pragma once

// m13 item 5c — the army-editor model: per-faction unit counts and
// spawn anchors over a base scenario, with the validation rules
// (per-faction minimum one unit, the 20k cap, anchors on the field).
// Pure data + pure functions; the window renders it, the setup screen
// feeds clicks into it, and to_scenario turns it into a savable file.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sim/scenario.hpp"
#include "sim/unit_types.hpp"

namespace game {

struct SetupEdit {
  // Per faction, one count per type of the table (table order).
  std::array<std::vector<std::uint32_t>, 2> counts{};
  std::array<std::optional<sim::SpawnAnchor>, 2> anchors{};
};

// The edit for a scenario's current armies (count-form factions only;
// a budget-form faction starts with its resolved composition so the
// editor always edits counts).
[[nodiscard]] SetupEdit setup_from_scenario(
    const sim::Scenario& scenario, const sim::UnitTypeTable& types,
    const std::array<std::vector<sim::TypeCount>, 2>& composition);

// One +/- step for a faction's type (faction: 0 = red, 1 = blue;
// delta: -1 / +1). Returns false when the step would leave that
// faction empty (the last unit of the only populated type) or push the
// battle over the unit cap; the edit is unchanged then.
[[nodiscard]] bool setup_step(SetupEdit& edit, std::size_t faction,
                              std::size_t type_index, int delta,
                              std::size_t unit_cap);

// Clamps an anchor to the battlefield (the drag can overshoot).
[[nodiscard]] sim::SpawnAnchor clamped_anchor(double x, double y,
                                              double world_w,
                                              double world_h);

// The edited scenario: the base with count-form armies and the anchors
// replaced. Objectives, squads, map and spawn patterns pass through.
[[nodiscard]] sim::Scenario to_scenario(
    const sim::Scenario& base, const SetupEdit& edit,
    const sim::UnitTypeTable& types);

}  // namespace game
