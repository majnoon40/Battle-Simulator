#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "sim/unit.hpp"

namespace sim {

// One personality archetype (M4), loaded from data/personalities.json. Each
// trait is a [min, max] range within [0, 1]; a unit assigned this archetype
// draws each trait uniformly from its range using the personality hash.
// `spawn_weight` is the relative probability that a unit receives this
// archetype (weights may be 0, but their sum must be positive).
struct PersonalityArchetype {
  std::string id;  // e.g. "brave"
  double bravery_min = 0.0;
  double bravery_max = 1.0;
  double caution_min = 0.0;
  double caution_max = 1.0;
  double aggression_min = 0.0;
  double aggression_max = 1.0;
  double discipline_min = 0.0;  // M6: order-obedience trait range
  double discipline_max = 1.0;
  double spawn_weight = 1.0;
};

// The full archetype table, kept in file order.
struct PersonalityTable {
  std::vector<PersonalityArchetype> archetypes;

  [[nodiscard]] std::size_t size() const noexcept {
    return archetypes.size();
  }
  [[nodiscard]] bool empty() const noexcept { return archetypes.empty(); }
  // Index of the archetype with this id, or -1.
  [[nodiscard]] int index_of(std::string_view id) const noexcept;
};

// Parses a personality table from JSON text. Expected shape (see
// data/personalities.json): {"archetypes": [{"id": ..., "traits": {"bravery":
// [min, max], "caution": [...], "aggression": [...], "discipline": [...]},
// "spawn_weight": ...}, ...]}. Throws std::invalid_argument with a descriptive
// message for every invalid input — malformed JSON, wrong root type, missing
// or unknown fields, non-string/empty ids, duplicate ids, ranges that are not
// two numbers with 0 <= min <= max <= 1, non-finite or negative spawn
// weights, an empty table, or an all-zero weight sum. Never silently
// defaults. Performs no file I/O.
[[nodiscard]] PersonalityTable parse_personalities(std::string_view json_text);

// Same parsing and validation, reading from any std::istream.
[[nodiscard]] PersonalityTable parse_personalities(std::istream& stream);

// The built-in default table used when no personality file is configured —
// identical to the shipped data/personalities.json (brave, cautious,
// aggressive, balanced, equal weights). Static storage; do not modify.
[[nodiscard]] const PersonalityTable& default_personality_table() noexcept;

// Deterministic personality for one unit: a SplitMix64-style hash of
// (seed, unit id) seeds a private generator that first draws the archetype
// (weighted) and then bravery, caution, aggression and discipline uniformly
// from the archetype's ranges, in that order. Discipline (M6) is the fourth
// draw, appended after the M4 stream, so bravery, caution and aggression are
// bit-identical to M4/M5 for every (seed, id). Pure integer/float arithmetic
// — the spawn RNG stream is never touched, so spawn positions are identical
// in every AI mode, and the same (seed, id, table) always yields the same
// personality on every platform.
[[nodiscard]] Personality personality_for(std::uint64_t seed,
                                          std::uint32_t unit_id,
                                          const PersonalityTable& table);

}  // namespace sim
