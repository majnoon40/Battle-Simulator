#include "sim/unit_types.hpp"

#include <cmath>
#include <cstdint>
#include <istream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace sim {
namespace {

[[noreturn]] void fail(std::string message) {
  throw std::invalid_argument(std::move(message));
}

// Reads a required field from a type object; `type_id` is used to name the
// offending type in the error message.
const nlohmann::json& required_field(const nlohmann::json& object,
                                     const std::string& type_id,
                                     std::string_view field) {
  const auto it = object.find(field);
  if (it == object.end()) {
    fail("unit type '" + type_id + "' is missing required field '" +
         std::string(field) + "'");
  }
  return *it;
}

// Strictly positive, finite number (> 0; zero is rejected by design so that
// a typo'd stat can never produce a helpless unit).
double positive_number(const nlohmann::json& value, const std::string& type_id,
                       std::string_view field) {
  if (!value.is_number()) {
    fail("unit type '" + type_id + "': field '" + std::string(field) +
         "' must be a number");
  }
  const double number = value.get<double>();
  if (!std::isfinite(number)) {
    fail("unit type '" + type_id + "': field '" + std::string(field) +
         "' must be a finite number");
  }
  if (!(number > 0.0)) {
    fail("unit type '" + type_id + "': field '" + std::string(field) +
         "' must be > 0 (got " + value.dump() + ")");
  }
  return number;
}

// Whole number of ticks, >= 1, fitting in std::uint32_t.
std::uint32_t positive_cooldown(const nlohmann::json& value,
                                const std::string& type_id) {
  if (!value.is_number_integer()) {
    fail("unit type '" + type_id +
         "': field 'cooldown' must be a whole number of ticks (got " +
         value.dump() + ")");
  }
  const std::int64_t ticks = value.get<std::int64_t>();
  if (ticks < 1 || ticks > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
    fail("unit type '" + type_id + "': field 'cooldown' must be >= 1 (got " +
         value.dump() + ")");
  }
  return static_cast<std::uint32_t>(ticks);
}

bool is_known_field(std::string_view field) {
  return field == "id" || field == "health" || field == "damage" ||
         field == "speed" || field == "range" || field == "cooldown" ||
         field == "sight_radius" || field == "leadership_radius" ||
         field == "accuracy" || field == "damage_variance" ||
         field == "cooldown_jitter";
}

UnitTypeTable parse_table(const nlohmann::json& root) {
  if (!root.is_array()) {
    fail("unit type table must be a JSON array of type objects");
  }
  if (root.empty()) {
    fail("unit type table must contain at least one type");
  }

  UnitTypeTable table;
  table.types.reserve(root.size());
  std::unordered_set<std::string> seen_ids;
  for (const nlohmann::json& entry : root) {
    if (!entry.is_object()) {
      fail("every unit type must be a JSON object (got " + entry.dump() + ")");
    }
    for (auto it = entry.begin(); it != entry.end(); ++it) {
      if (!is_known_field(it.key())) {
        fail("unknown field '" + it.key() +
             "' in unit type entry (expected id, health, damage, speed, "
             "range, cooldown, sight_radius)");
      }
    }

    if (auto it = entry.find("id"); it == entry.end()) {
      fail("unit type entry is missing required field 'id'");
    } else if (!it->is_string()) {
      fail("unit type field 'id' must be a string (got " + it->dump() + ")");
    }
    const std::string id = entry["id"].get<std::string>();
    if (id.empty()) {
      fail("unit type id must not be empty");
    }

    UnitType type;
    type.id = id;
    type.stats.max_health =
        positive_number(required_field(entry, id, "health"), id, "health");
    type.stats.damage =
        positive_number(required_field(entry, id, "damage"), id, "damage");
    type.stats.speed =
        positive_number(required_field(entry, id, "speed"), id, "speed");
    type.stats.attack_range =
        positive_number(required_field(entry, id, "range"), id, "range");
    type.stats.attack_cooldown_ticks =
        positive_cooldown(required_field(entry, id, "cooldown"), id);
    const double sight_radius =
        positive_number(required_field(entry, id, "sight_radius"), id,
                        "sight_radius");
    if (!(sight_radius > type.stats.attack_range)) {
      fail("unit type '" + id + "': sight_radius must strictly exceed the "
           "attack range (sight " + std::to_string(sight_radius) +
           ", range " + std::to_string(type.stats.attack_range) + ")");
    }
    type.stats.sight_radius = sight_radius;
    // Command radius (M6): optional; absent means "cannot command" (0).
    // Present values must be strictly positive — a commander with no reach
    // is a data error, not a default.
    if (auto it = entry.find("leadership_radius"); it != entry.end()) {
      type.stats.leadership_radius =
          positive_number(*it, id, "leadership_radius");
    }
    // Combat variance (M10): optional fractions in [0, 1]. Absent values
    // default to the M1-M9 deterministic combat (accuracy 1.0, no
    // spread), so existing type files keep their exact old behavior.
    if (auto it = entry.find("accuracy"); it != entry.end()) {
      if (!it->is_number() || !(it->get<double>() >= 0.0) ||
          !(it->get<double>() <= 1.0)) {
        fail("unit type '" + id + "': field 'accuracy' must be a number in "
             "[0, 1]");
      }
      type.stats.accuracy = it->get<double>();
    }
    for (const char* fraction_field : {"damage_variance", "cooldown_jitter"}) {
      if (auto it = entry.find(fraction_field); it != entry.end()) {
        if (!it->is_number() || !(it->get<double>() >= 0.0) ||
            !(it->get<double>() <= 1.0)) {
          fail(std::string("unit type '") + id + "': field '" +
               fraction_field + "' must be a number in [0, 1]");
        }
        if (fraction_field == std::string("damage_variance")) {
          type.stats.damage_variance = it->get<double>();
        } else {
          type.stats.cooldown_jitter = it->get<double>();
        }
      }
    }

    if (!seen_ids.insert(id).second) {
      fail("duplicate unit type id '" + id + "'");
    }
    table.types.push_back(std::move(type));
  }
  return table;
}

}  // namespace

int UnitTypeTable::index_of(std::string_view id) const noexcept {
  for (std::size_t i = 0; i < types.size(); ++i) {
    if (types[i].id == id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

UnitTypeTable parse_unit_types(std::string_view json_text) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(json_text.begin(), json_text.end());
  } catch (const nlohmann::json::exception& error) {
    fail(std::string("not valid JSON: ") + error.what());
  }
  return parse_table(root);
}

UnitTypeTable parse_unit_types(std::istream& stream) {
  if (!stream.good()) {
    fail("unit type table input stream is not readable");
  }
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(stream);
  } catch (const nlohmann::json::exception& error) {
    fail(std::string("not valid JSON: ") + error.what());
  }
  return parse_table(root);
}

}  // namespace sim
