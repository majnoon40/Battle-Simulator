#include "sim/personalities.hpp"

#include <cmath>
#include <cstdint>
#include <istream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "sim/rng.hpp"
#include "sim/unit.hpp"

namespace sim {
namespace {

[[noreturn]] void fail(std::string message) {
  throw std::invalid_argument(std::move(message));
}

const nlohmann::json& required_field(const nlohmann::json& object,
                                     const std::string& owner,
                                     std::string_view field) {
  const auto it = object.find(field);
  if (it == object.end()) {
    fail(owner + " is missing required field '" + std::string(field) + "'");
  }
  return *it;
}

// A finite number (any sign; callers apply their own bounds).
double finite_number(const nlohmann::json& value, const std::string& owner,
                     std::string_view field) {
  if (!value.is_number()) {
    fail(owner + ": field '" + std::string(field) + "' must be a number");
  }
  const double number = value.get<double>();
  if (!std::isfinite(number)) {
    fail(owner + ": field '" + std::string(field) +
         "' must be a finite number");
  }
  return number;
}

// A [min, max] trait range: two numbers with 0 <= min <= max <= 1.
void trait_range(const nlohmann::json& value, const std::string& owner,
                 const std::string& trait, double& min, double& max) {
  if (!value.is_array() || value.size() != 2) {
    fail(owner + ": trait '" + trait + "' must be a [min, max] array");
  }
  min = finite_number(value[0], owner, trait);
  max = finite_number(value[1], owner, trait);
  if (!(min >= 0.0) || !(min <= max) || !(max <= 1.0)) {
    fail(owner + ": trait '" + trait + "' must satisfy 0 <= min <= max <= 1");
  }
}

PersonalityTable parse_table(const nlohmann::json& root) {
  if (!root.is_object()) {
    fail("personality table must be a JSON object");
  }
  for (auto it = root.begin(); it != root.end(); ++it) {
    if (it.key() != "archetypes") {
      fail("unknown field '" + it.key() +
           "' in personality table (expected archetypes)");
    }
  }
  const nlohmann::json& archetypes = required_field(root, "personality table",
                                                    "archetypes");
  if (!archetypes.is_array() || archetypes.empty()) {
    fail("personality table must contain at least one archetype");
  }
  if (archetypes.size() > 255) {
    fail("personality table must contain at most 255 archetypes");
  }

  PersonalityTable table;
  table.archetypes.reserve(archetypes.size());
  double total_weight = 0.0;
  for (const nlohmann::json& entry : archetypes) {
    if (!entry.is_object()) {
      fail("every personality archetype must be a JSON object");
    }
    for (auto it = entry.begin(); it != entry.end(); ++it) {
      if (it.key() != "id" && it.key() != "traits" &&
          it.key() != "spawn_weight") {
        fail("unknown field '" + it.key() +
             "' in personality archetype (expected id, traits, "
             "spawn_weight)");
      }
    }
    const nlohmann::json& id_json = required_field(entry, "personality archetype",
                                                   "id");
    if (!id_json.is_string() || id_json.get<std::string>().empty()) {
      fail("personality archetype id must be a non-empty string");
    }
    const std::string id = id_json.get<std::string>();
    const std::string owner = "personality archetype '" + id + "'";

    PersonalityArchetype archetype;
    archetype.id = id;

    const nlohmann::json& traits =
        required_field(entry, owner, "traits");
    if (!traits.is_object()) {
      fail(owner + ": 'traits' must be an object");
    }
    for (auto it = traits.begin(); it != traits.end(); ++it) {
      if (it.key() != "bravery" && it.key() != "caution" &&
          it.key() != "aggression" && it.key() != "discipline") {
        fail(owner + ": unknown trait '" + it.key() +
             "' (expected bravery, caution, aggression, discipline)");
      }
    }
    trait_range(required_field(traits, owner, "bravery"), owner, "bravery",
                archetype.bravery_min, archetype.bravery_max);
    trait_range(required_field(traits, owner, "caution"), owner, "caution",
                archetype.caution_min, archetype.caution_max);
    trait_range(required_field(traits, owner, "aggression"), owner,
                "aggression", archetype.aggression_min,
                archetype.aggression_max);
    trait_range(required_field(traits, owner, "discipline"), owner,
                "discipline", archetype.discipline_min,
                archetype.discipline_max);

    archetype.spawn_weight =
        finite_number(required_field(entry, owner, "spawn_weight"), owner,
                      "spawn_weight");
    if (!(archetype.spawn_weight >= 0.0)) {
      fail(owner + ": spawn_weight must be >= 0");
    }
    total_weight += archetype.spawn_weight;

    if (table.index_of(id) >= 0) {
      fail("duplicate personality archetype id '" + id + "'");
    }
    table.archetypes.push_back(std::move(archetype));
  }
  if (!(total_weight > 0.0)) {
    fail("personality table needs at least one archetype with spawn_weight > 0");
  }
  return table;
}

}  // namespace

int PersonalityTable::index_of(std::string_view id) const noexcept {
  for (std::size_t i = 0; i < archetypes.size(); ++i) {
    if (archetypes[i].id == id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

PersonalityTable parse_personalities(std::string_view json_text) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(json_text.begin(), json_text.end());
  } catch (const nlohmann::json::exception& error) {
    fail(std::string("not valid JSON: ") + error.what());
  }
  return parse_table(root);
}

PersonalityTable parse_personalities(std::istream& stream) {
  if (!stream.good()) {
    fail("personality table input stream is not readable");
  }
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(stream);
  } catch (const nlohmann::json::exception& error) {
    fail(std::string("not valid JSON: ") + error.what());
  }
  return parse_table(root);
}

const PersonalityTable& default_personality_table() noexcept {
  static const PersonalityTable table = [] {
    PersonalityTable table;
    auto add = [&](const char* id, double b0, double b1, double c0, double c1,
                   double a0, double a1, double d0, double d1) {
      PersonalityArchetype archetype;
      archetype.id = id;
      archetype.bravery_min = b0;
      archetype.bravery_max = b1;
      archetype.caution_min = c0;
      archetype.caution_max = c1;
      archetype.aggression_min = a0;
      archetype.aggression_max = a1;
      archetype.discipline_min = d0;
      archetype.discipline_max = d1;
      archetype.spawn_weight = 1.0;
      table.archetypes.push_back(std::move(archetype));
    };
    add("brave", 0.75, 1.0, 0.0, 0.25, 0.5, 1.0, 0.4, 0.8);
    add("cautious", 0.0, 0.25, 0.75, 1.0, 0.0, 0.4, 0.6, 1.0);
    add("aggressive", 0.5, 0.9, 0.0, 0.3, 0.8, 1.0, 0.3, 0.7);
    add("balanced", 0.35, 0.65, 0.35, 0.65, 0.35, 0.65, 0.35, 0.65);
    return table;
  }();
  return table;
}

// SplitMix64 finalizer over (seed, unit id): plain 64-bit integer
// arithmetic, identical on every platform.
Personality personality_for(std::uint64_t seed, std::uint32_t unit_id,
                            const PersonalityTable& table) {
  Personality personality;
  if (table.empty()) {
    return personality;  // defensive; validation rejects empty tables
  }
  std::uint64_t z =
      seed + 0x9E3779B97F4A7C15ULL * static_cast<std::uint64_t>(unit_id);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  z ^= z >> 31;

  // Draw order is fixed and documented: archetype (weighted), then bravery,
  // caution, aggression, discipline — each uniform in its archetype range.
  // Discipline is the fourth draw (M6): the first three draws are untouched,
  // so M4/M5 personalities keep their exact values.
  Rng rng(z);
  double total_weight = 0.0;
  for (const PersonalityArchetype& archetype : table.archetypes) {
    total_weight += archetype.spawn_weight;
  }
  double roll = rng.next_double() * total_weight;
  const PersonalityArchetype* chosen = &table.archetypes.back();
  for (const PersonalityArchetype& archetype : table.archetypes) {
    if (roll < archetype.spawn_weight) {
      chosen = &archetype;
      break;
    }
    roll -= archetype.spawn_weight;
  }
  personality.archetype = static_cast<std::uint8_t>(
      static_cast<std::size_t>(chosen - table.archetypes.data()));
  personality.bravery =
      chosen->bravery_min + rng.next_double() * (chosen->bravery_max -
                                                 chosen->bravery_min);
  personality.caution =
      chosen->caution_min + rng.next_double() * (chosen->caution_max -
                                                 chosen->caution_min);
  personality.aggression =
      chosen->aggression_min + rng.next_double() * (chosen->aggression_max -
                                                    chosen->aggression_min);
  personality.discipline =
      chosen->discipline_min + rng.next_double() * (chosen->discipline_max -
                                                    chosen->discipline_min);
  return personality;
}

}  // namespace sim
