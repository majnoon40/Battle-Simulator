#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sim/factions.hpp"
#include "sim/unit_types.hpp"

namespace sim {

// One army composition entry exactly as written in a scenario file: `count`
// units of the type named `type_id`. Entries keep file order. M10: a
// two-element array [min, max] instead of a fixed number makes the count a
// per-seed draw (counter RNG channel 4) in [count_min, count_max]; such an
// entry carries count = count_min until build_composition resolves it.
struct ScenarioEntry {
  std::string type_id;
  std::uint32_t count = 0;
  std::uint32_t count_min = 0;  // set when the file gives a range
  std::uint32_t count_max = 0;  // 0 = fixed count (no range)
};

// m13 item H(b): a budget-form army, as written in a scenario file
// ("armies": {"red": {"budget": 400, "mix": {"infantry": 3, "archer":
// 2}}}). The faction spends `budget` points on units, each unit of a
// type costing that type's `cost` field (data/units.json, default 1);
// the points are split across the mix proportional to the integer
// weights. The fill is deterministic (no seed): counts start at the
// floor of each share, every mix type fields at least one unit, and
// leftover points go to the largest affordable fractional remainders
// in mix order. An army is either count-form or budget-form, never
// both.
struct ScenarioBudget {
  double budget = 0.0;             // total army points, > 0
  std::vector<ScenarioEntry> mix;  // type_id + weight (count reused)
};

// A per-faction objective point (M4), as written in a scenario file.
struct ScenarioObjective {
  double x = 0.0;
  double y = 0.0;
};

// A per-faction squad plan (M6), as written in a scenario file: `count`
// squads, each of `size` soldiers plus one commander (a unit of a commander
// type). `size` may be omitted in the file — the documented default is 25.
struct ScenarioSquads {
  std::uint32_t count = 0;
  std::uint32_t size = 25;
};

// A battle scenario: battlefield size, spawn band and the per-faction army
// composition (see data/scenarios/*.json for the shape). Type names are
// resolved against a UnitTypeTable by build_composition(). The optional
// per-faction objectives override the default (the center of the opposing
// spawn band). The optional per-faction squad plans (M6) organize that
// faction's army into squads; a faction without a plan fields no squads even
// when squads are enabled.
// A map reference (M8): the map file (a PNG under data/maps/ with exact
// RGB terrain colors, or a JSON grid) and the cell size in world units.
// When present, the battlefield size is map size x cell_size and terrain
// rules apply.
struct ScenarioMap {
  std::string file;
  double cell_size = 0.0;
};

struct Scenario {
  double battlefield_width = 0.0;
  double battlefield_height = 0.0;
  double spawn_band_fraction = 0.0;
  std::optional<ScenarioMap> map{};
  std::array<std::vector<ScenarioEntry>, kFactionCount> armies{};
  // m13 item H(b): budget-form armies. When set for a faction, that
  // faction's `armies` entry list is empty and the composition comes
  // from the budget fill in build_composition().
  std::array<std::optional<ScenarioBudget>, kFactionCount> budgets{};
  std::array<std::optional<ScenarioObjective>, kFactionCount> objectives{};
  std::array<std::optional<ScenarioSquads>, kFactionCount> squads{};
  // M10 spawn patterns: Band (default) keeps the M1-M9 uniform random
  // spawn; line / columns / echelon / cluster are deterministic opt-ins
  // ("spawn_pattern": "..." for both factions, or "spawn_patterns":
  // {"red": ..., "blue": ...}).
  std::array<SpawnPattern, kFactionCount> spawn_patterns{};
};

// Parses a scenario from JSON text. Expected shape (see
// data/scenarios/default.json): an object with "battlefield" {width, height},
// "spawn_band_fraction", "armies" {red, blue} and the optional "objectives"
// {red, blue} of {x, y} points on the battlefield (factions may be omitted
// individually) and "squads" {red, blue} of {count, size?} plans (factions may
// be omitted individually; size defaults to 25). Each army is a non-empty
// object of type-id -> count. Throws std::invalid_argument with a descriptive
// message for every invalid input — malformed JSON, wrong types, missing or
// unknown fields, non-positive dimensions, spawn band outside (0, 0.5],
// non-integer or zero/negative counts, empty armies, totals above the unit
// cap, objectives off the battlefield, or squad counts/sizes below 1. Never
// silently defaults (beyond the documented squad-size default). Performs no
// file I/O.
[[nodiscard]] Scenario parse_scenario(std::string_view json_text);

// Same parsing and validation, reading from any std::istream.
[[nodiscard]] Scenario parse_scenario(std::istream& stream);

// Resolves the scenario's type names against the table and returns the
// indexed compositions in file order (armies[Red], armies[Blue]). Throws
// std::invalid_argument naming the first unknown type. The per-army and
// total unit caps were already enforced by parse_scenario. M10: entries
// written as [min, max] ranges draw their count from the counter RNG
// (channel 4, keyed by seed, faction and entry), so the same seed always
// yields the same army sizes. `seed` defaults to 0 for compatibility with
// fixed-count scenarios; ranged entries resolve identically every call
// for a given seed.
[[nodiscard]] std::array<std::vector<TypeCount>, kFactionCount>
build_composition(const Scenario& scenario, const UnitTypeTable& types,
                  std::uint64_t seed = 0);

}  // namespace sim
