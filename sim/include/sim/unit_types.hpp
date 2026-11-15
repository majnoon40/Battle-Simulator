#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "sim/unit.hpp"

namespace sim {

// One unit archetype, loaded from data (units.json). The stat values are
// copied into every unit of this type at spawn time, exactly like the M1
// per-faction stats were. `sight_radius` (M4) must strictly exceed the type's
// attack range so a unit can always see what it can shoot at.
struct UnitType {
  std::string id;  // stable type identifier, e.g. "infantry"
  UnitStats stats{};
};

// Indexed spawn-plan entry: `count` units of table type `type_index`.
struct TypeCount {
  std::uint32_t type_index = 0;
  std::uint32_t count = 0;
};

// The full unit type table, kept in file order. Tables are tiny, so id
// lookups are a linear scan; indices stay valid for the table's lifetime.
struct UnitTypeTable {
  std::vector<UnitType> types;

  [[nodiscard]] std::size_t size() const noexcept { return types.size(); }
  [[nodiscard]] bool empty() const noexcept { return types.empty(); }
  // Index of the type with this id, or -1 when the table has no such type.
  [[nodiscard]] int index_of(std::string_view id) const noexcept;
};

// Parses a unit type table from JSON text. Expected shape (see
// data/units.json): a non-empty array of objects with the fields id, health,
// damage, speed, range, cooldown, sight_radius and the optional
// leadership_radius (M6; strictly positive when present, 0 when absent —
// only commander types carry it). Throws std::invalid_argument with a
// descriptive message for every invalid input — malformed JSON, wrong root
// type, missing or unknown fields, non-numeric or non-positive values,
// non-integer cooldowns, empty ids, duplicate ids, an empty table, or a
// sight radius that does not strictly exceed the type's attack range. Never
// silently defaults. Performs no file I/O; callers read the text themselves
// (or use the stream overload below).
[[nodiscard]] UnitTypeTable parse_unit_types(std::string_view json_text);

// Same parsing and validation, reading from any std::istream (the stream must
// be open and readable).
[[nodiscard]] UnitTypeTable parse_unit_types(std::istream& stream);

}  // namespace sim
