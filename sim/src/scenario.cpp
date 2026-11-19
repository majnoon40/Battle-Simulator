#include "sim/scenario.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <istream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "sim/config.hpp"
#include "sim/factions.hpp"

namespace sim {
namespace {

[[noreturn]] void fail(std::string message) {
  throw std::invalid_argument(std::move(message));
}

// nlohmann::ordered_json keeps object keys in file order, so army
// composition entries survive in the order the author wrote them (this is the
// spawn order). Values are then read out explicitly, so the ordering behavior
// of plain json objects never matters here.
using OrderedJson = nlohmann::ordered_json;

const OrderedJson& required_field(const OrderedJson& object,
                                  std::string_view field,
                                  const std::string& where) {
  const auto it = object.find(field);
  if (it == object.end()) {
    fail(where + " is missing required field '" + std::string(field) + "'");
  }
  return *it;
}

double positive_number(const OrderedJson& value, std::string_view field,
                       const std::string& where) {
  if (!value.is_number()) {
    fail(where + ": field '" + std::string(field) + "' must be a number (got " +
         value.dump() + ")");
  }
  const double number = value.get<double>();
  if (!std::isfinite(number) || !(number > 0.0)) {
    fail(where + ": field '" + std::string(field) + "' must be > 0 (got " +
         value.dump() + ")");
  }
  return number;
}

// A finite number >= 0 (battlefield coordinates may sit on the edge).
double non_negative_number(const OrderedJson& value, std::string_view field,
                           const std::string& where) {
  if (!value.is_number()) {
    fail(where + ": field '" + std::string(field) + "' must be a number (got " +
         value.dump() + ")");
  }
  const double number = value.get<double>();
  if (!std::isfinite(number) || !(number >= 0.0)) {
    fail(where + ": field '" + std::string(field) + "' must be >= 0 (got " +
         value.dump() + ")");
  }
  return number;
}

const char* spawn_pattern_name(SpawnPattern pattern) {
  switch (pattern) {
    case SpawnPattern::Band: return "band";
    case SpawnPattern::Line: return "line";
    case SpawnPattern::Columns: return "columns";
    case SpawnPattern::Echelon: return "echelon";
    case SpawnPattern::Cluster: return "cluster";
  }
  return "unknown";
}

SpawnPattern parse_spawn_pattern(const OrderedJson& value,
                                 const std::string& where) {
  if (!value.is_string()) {
    fail(where + " must be one of band, line, columns, echelon, cluster");
  }
  const std::string text = value.get<std::string>();
  for (const SpawnPattern pattern :
       {SpawnPattern::Band, SpawnPattern::Line, SpawnPattern::Columns,
        SpawnPattern::Echelon, SpawnPattern::Cluster}) {
    if (text == spawn_pattern_name(pattern)) {
      return pattern;
    }
  }
  fail(where + " '" + text +
       "' is unknown (expected band, line, columns, echelon, cluster)");
}

std::uint32_t positive_count(const OrderedJson& value, const std::string& type_id,
                             const std::string& army_name) {
  if (!value.is_number_integer()) {
    fail("army '" + army_name + "': count for unit type '" + type_id +
         "' must be a whole number (got " + value.dump() + ")");
  }
  const std::int64_t count = value.get<std::int64_t>();
  if (count < 1) {
    fail("army '" + army_name + "': count for unit type '" + type_id +
         "' must be >= 1 (got " + value.dump() + ")");
  }
  if (count > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
    fail("army '" + army_name + "': count for unit type '" + type_id +
         "' is out of range");
  }
  return static_cast<std::uint32_t>(count);
}

std::vector<ScenarioEntry> parse_army(
    const OrderedJson& armies, std::string_view key,
    std::optional<ScenarioBudget>& budget,
    std::optional<SpawnAnchor>& anchor, double world_w, double world_h) {
  const std::string name(key);
  const OrderedJson& army = required_field(armies, key, "'armies'");
  if (!army.is_object()) {
    fail("army '" + name + "' must be a JSON object of type -> count");
  }
  if (army.empty()) {
    fail("army '" + name + "' must list at least one unit type");
  }
  // m13 item H(b): budget form. {"budget": points, "mix": {type: weight}}
  // replaces the count entries for this army; the two forms are mutually
  // exclusive so a scenario never has two ways to say the same thing.
  const bool has_budget = army.contains("budget");
  const bool has_mix = army.contains("mix");
  // m13 item 5c: the optional spawn anchor works in both army forms.
  if (const auto anchor_field = army.find("anchor");
      anchor_field != army.end()) {
    if (!anchor_field->is_array() || anchor_field->size() != 2 ||
        !(*anchor_field)[0].is_number() || !(*anchor_field)[1].is_number()) {
      fail("army '" + name + "': field 'anchor' must be [x, y] numbers");
    }
    SpawnAnchor parsed;
    parsed.x = (*anchor_field)[0].get<double>();
    parsed.y = (*anchor_field)[1].get<double>();
    if (!std::isfinite(parsed.x) || !std::isfinite(parsed.y) ||
        parsed.x < 0.0 || parsed.x > world_w ||
        parsed.y < 0.0 || parsed.y > world_h) {
      fail("army '" + name +
           "': anchor must lie on the battlefield (got " +
           anchor_field->dump() + ")");
    }
    anchor = parsed;
  }
  if (has_budget || has_mix) {
    if (!has_budget || !has_mix) {
      fail("army '" + name +
           "': budget-form armies need both 'budget' and 'mix'");
    }
    if (army.size() != 2 &&
        !(army.size() == 3 && army.contains("anchor"))) {
      fail("army '" + name +
           "': budget-form armies take exactly 'budget' and 'mix' "
           "(plus the optional 'anchor'; counts and budget are mutually "
           "exclusive)");
    }
    const OrderedJson& budget_value = army.at("budget");
    if (!budget_value.is_number()) {
      fail("army '" + name + "': field 'budget' must be a number (got " +
           budget_value.dump() + ")");
    }
    const double points = budget_value.get<double>();
    if (!std::isfinite(points) || points <= 0.0) {
      fail("army '" + name + "': field 'budget' must be > 0 (got " +
           budget_value.dump() + ")");
    }
    const OrderedJson& mix = army.at("mix");
    if (!mix.is_object() || mix.empty()) {
      fail("army '" + name + "' field 'mix' must be a non-empty object of "
           "type -> weight");
    }
    ScenarioBudget spec;
    spec.budget = points;
    spec.mix.reserve(mix.size());
    for (auto it = mix.begin(); it != mix.end(); ++it) {
      if (it.key().empty()) {
        fail("army '" + name + "' has an empty unit type id in 'mix'");
      }
      ScenarioEntry entry;
      entry.type_id = it.key();
      // Weights reuse the count validation: positive integers within
      // the unit cap (a weight above the cap could never be spent).
      entry.count = positive_count(it.value(), it.key(), name);
      spec.mix.push_back(entry);
    }
    budget = spec;
    return {};
  }

  std::vector<ScenarioEntry> entries;
  entries.reserve(army.size());
  std::uint64_t army_total = 0;
  for (auto it = army.begin(); it != army.end(); ++it) {
    if (it.key() == "anchor") {
      continue;  // m13 item 5c: handled above / below for both forms
    }
    if (it.key().empty()) {
      fail("army '" + name + "' has an empty unit type id");
    }
    ScenarioEntry entry;
    entry.type_id = it.key();
    if (it.value().is_array()) {
      // M10 composition range: [min, max], drawn per seed in
      // build_composition. Validation uses the maximum.
      if (it.value().size() != 2 || !it.value()[0].is_number_integer() ||
          !it.value()[1].is_number_integer()) {
        fail("army '" + name + "': count for unit type '" + it.key() +
             "' must be a number or a [min, max] array of integers");
      }
      const std::int64_t lo = it.value()[0].get<std::int64_t>();
      const std::int64_t hi = it.value()[1].get<std::int64_t>();
      if (lo < 0 || hi < lo) {
        fail("army '" + name + "': range for unit type '" + it.key() +
             "' must satisfy 0 <= min <= max");
      }
      if (hi > static_cast<std::int64_t>(kMaxUnits)) {
        fail("army '" + name + "': range maximum for unit type '" + it.key() +
             "' exceeds the unit cap");
      }
      entry.count = static_cast<std::uint32_t>(lo);
      entry.count_min = static_cast<std::uint32_t>(lo);
      entry.count_max = static_cast<std::uint32_t>(hi);
      army_total += static_cast<std::uint64_t>(hi);
    } else {
      entry.count = positive_count(it.value(), it.key(), name);
      army_total += entry.count;
    }
    if (army_total > kMaxUnits) {
      fail("army '" + name + "' needs more than " +
           std::to_string(kMaxUnits) + " units, which exceeds the unit cap");
    }
    entries.push_back(entry);
  }
  return entries;
}

Scenario parse_scenario_document(const OrderedJson& root) {
  if (!root.is_object()) {
    fail("scenario must be a JSON object");
  }
  for (auto it = root.begin(); it != root.end(); ++it) {
    if (it.key() != "battlefield" && it.key() != "spawn_band_fraction" &&
        it.key() != "armies" && it.key() != "objectives" &&
        it.key() != "squads" && it.key() != "map" &&
        it.key() != "spawn_pattern" && it.key() != "spawn_patterns") {
      fail("unknown scenario field '" + it.key() +
           "' (expected battlefield, spawn_band_fraction, armies, objectives, "
           "squads, map, spawn_pattern, spawn_patterns)");
    }
  }

  const OrderedJson& battlefield = required_field(root, "battlefield", "scenario");
  if (!battlefield.is_object()) {
    fail("scenario field 'battlefield' must be a JSON object");
  }
  for (auto it = battlefield.begin(); it != battlefield.end(); ++it) {
    if (it.key() != "width" && it.key() != "height") {
      fail("unknown battlefield field '" + it.key() + "' (expected width, height)");
    }
  }

  Scenario scenario;
  scenario.battlefield_width =
      positive_number(required_field(battlefield, "width", "'battlefield'"),
                      "width", "battlefield");
  scenario.battlefield_height =
      positive_number(required_field(battlefield, "height", "'battlefield'"),
                      "height", "battlefield");

  const OrderedJson& band = required_field(root, "spawn_band_fraction", "scenario");
  if (!band.is_number()) {
    fail("scenario field 'spawn_band_fraction' must be a number (got " +
         band.dump() + ")");
  }
  scenario.spawn_band_fraction = band.get<double>();
  if (!(scenario.spawn_band_fraction > 0.0) ||
      !(scenario.spawn_band_fraction <= 0.5)) {
    fail("scenario field 'spawn_band_fraction' must be in (0, 0.5] so the "
         "armies start apart (got " +
         band.dump() + ")");
  }

  const OrderedJson& armies = required_field(root, "armies", "scenario");
  if (!armies.is_object()) {
    fail("scenario field 'armies' must be a JSON object");
  }
  // M14 item 2: armies may field red and blue (always required) plus any
  // contiguous set of the extra factions green..magenta. The keys map to
  // fixed Faction indices (never file order); faction_count = highest
  // index + 1, and every faction below it must have an army (a missing
  // key fails in parse_army with a field-level message).
  std::size_t max_index = static_cast<std::size_t>(Faction::Blue);
  for (auto it = armies.begin(); it != armies.end(); ++it) {
    const auto faction = faction_from_key(it.key());
    if (!faction) {
      fail("unknown armies faction '" + it.key() +
           "' (expected red, blue, green, yellow, purple, orange, cyan, magenta)");
    }
    max_index = std::max(max_index, static_cast<std::size_t>(*faction));
  }
  scenario.faction_count = static_cast<std::uint8_t>(max_index + 1);
  for (std::size_t index = 0; index <= max_index; ++index) {
    const std::string key{faction_key(static_cast<Faction>(index))};
    scenario.armies[index] =
        parse_army(armies, key, scenario.budgets[index],
                   scenario.anchors[index], scenario.battlefield_width,
                   scenario.battlefield_height);
  }

  // Optional per-faction objectives (M4): {"red": {"x": .., "y": ..},
  // "blue": {...}}. Factions may be omitted individually; both default to
  // the center of the opposing spawn band.
  if (const auto objectives = root.find("objectives");
      objectives != root.end()) {
    if (!objectives->is_object()) {
      fail("scenario field 'objectives' must be a JSON object");
    }
    for (auto it = objectives->begin(); it != objectives->end(); ++it) {
      const auto faction = faction_from_key(it.key());
      if (!faction ||
          static_cast<std::size_t>(*faction) >= scenario.faction_count) {
        fail("unknown objectives faction '" + it.key() +
             "' (expected the scenario's army factions)");
      }
      const std::size_t index = static_cast<std::size_t>(*faction);
      const OrderedJson& point = *it;
      if (!point.is_object()) {
        fail("objective '" + it.key() + "' must be a JSON object {x, y}");
      }
      for (auto field = point.begin(); field != point.end(); ++field) {
        if (field.key() != "x" && field.key() != "y") {
          fail("unknown objective field '" + field.key() + "' (expected x, y)");
        }
      }
      ScenarioObjective objective;
      objective.x = non_negative_number(
          required_field(point, "x", "objective '" + it.key() + "'"), "x",
          "objective '" + it.key() + "'");
      objective.y = non_negative_number(
          required_field(point, "y", "objective '" + it.key() + "'"), "y",
          "objective '" + it.key() + "'");
      if (objective.x > scenario.battlefield_width ||
          objective.y > scenario.battlefield_height) {
        fail("objective '" + it.key() +
             "' must lie on the battlefield [0, width] x [0, height]");
      }
      scenario.objectives[index] = objective;
    }
  }

  // Optional per-faction squad plans (M6): {"red": {"count": .., "size": ..},
  // "blue": {...}}. Factions may be omitted individually; a faction without a
  // plan fields no squads. `size` may be omitted (documented default 25);
  // `count` is required. The plan is validated against the army composition
  // when the simulation is constructed (it must exactly match: count
  // commander-type units and count*size soldiers).
  if (const auto squads = root.find("squads"); squads != root.end()) {
    if (!squads->is_object()) {
      fail("scenario field 'squads' must be a JSON object");
    }
    for (auto it = squads->begin(); it != squads->end(); ++it) {
      const auto faction = faction_from_key(it.key());
      if (!faction ||
          static_cast<std::size_t>(*faction) >= scenario.faction_count) {
        fail("unknown squads faction '" + it.key() +
             "' (expected the scenario's army factions)");
      }
      const std::size_t index = static_cast<std::size_t>(*faction);
      const OrderedJson& plan = *it;
      if (!plan.is_object()) {
        fail("squad plan '" + it.key() + "' must be a JSON object");
      }
      for (auto field = plan.begin(); field != plan.end(); ++field) {
        if (field.key() != "count" && field.key() != "size") {
          fail("unknown squad plan field '" + field.key() +
               "' (expected count, size)");
        }
      }
      const std::string where = "squad plan '" + it.key() + "'";
      const OrderedJson& count = required_field(plan, "count", where);
      if (!count.is_number_integer() || count.get<std::int64_t>() < 1) {
        fail(where + ": 'count' must be a whole number >= 1");
      }
      // m13 audit finding 4: validate against the destination range
      // before converting — the plan fields are uint32_t and the
      // previous unchecked static_cast truncated anything above 2^32-1
      // (4294967296 became 0). Same rule as the army `count` parser.
      if (count.get<std::int64_t>() >
          static_cast<std::int64_t>(
              std::numeric_limits<std::uint32_t>::max())) {
        fail(where + ": 'count' must not exceed " +
             std::to_string(
                 std::numeric_limits<std::uint32_t>::max()));
      }
      ScenarioSquads parsed;
      parsed.count = static_cast<std::uint32_t>(count.get<std::int64_t>());
      if (const auto size = plan.find("size"); size != plan.end()) {
        if (!size->is_number_integer() || size->get<std::int64_t>() < 1) {
          fail(where + ": 'size' must be a whole number >= 1");
        }
        if (size->get<std::int64_t>() >
            static_cast<std::int64_t>(
                std::numeric_limits<std::uint32_t>::max())) {
          fail(where + ": 'size' must not exceed " +
               std::to_string(
                   std::numeric_limits<std::uint32_t>::max()));
        }
        parsed.size = static_cast<std::uint32_t>(size->get<std::int64_t>());
      }
      scenario.squads[index] = parsed;
    }
  }

  // Optional terrain map reference (M8): {file, cell_size}. The loader
  // (cli/) resolves the file against data/maps/ (or a mod's maps/) and
  // derives the battlefield size from the map.
  if (root.contains("map")) {
    const OrderedJson& map = root["map"];
    if (!map.is_object()) {
      fail("scenario field 'map' must be a JSON object");
    }
    for (auto it = map.begin(); it != map.end(); ++it) {
      if (it.key() != "file" && it.key() != "cell_size") {
        fail("unknown map field '" + it.key() +
             "' (expected file, cell_size)");
      }
    }
    ScenarioMap parsed;
    if (!map.contains("file") || !map["file"].is_string() ||
        map["file"].get<std::string>().empty()) {
      fail("scenario field 'map.file' must be a non-empty string");
    }
    parsed.file = map["file"].get<std::string>();
    if (!map.contains("cell_size") || !map["cell_size"].is_number()) {
      fail("scenario field 'map.cell_size' must be a number > 0");
    }
    parsed.cell_size = map["cell_size"].get<double>();
    // Finite and positive (a NaN compares false to everything; +inf
    // passed the bare > 0 check and made the derived battlefield size
    // infinite).
    if (!std::isfinite(parsed.cell_size) || !(parsed.cell_size > 0.0)) {
      fail("scenario field 'map.cell_size' must be a finite number > 0");
    }
    scenario.map = parsed;
  }

  std::uint64_t total = 0;
  for (const auto& army : scenario.armies) {
    for (const ScenarioEntry& entry : army) {
      total += entry.count;
    }
  }
  if (total > kMaxUnits) {
    fail("scenario needs " + std::to_string(total) +
         " units in total, which exceeds the " + std::to_string(kMaxUnits) +
         "-unit cap");
  }
  // M10 spawn patterns (optional; Band default). "spawn_pattern" sets
  // every fielded faction, "spawn_patterns" sets them individually.
  if (auto it = root.find("spawn_pattern"); it != root.end()) {
    const SpawnPattern pattern =
        parse_spawn_pattern(*it, "scenario field 'spawn_pattern'");
    for (std::size_t index = 0; index < scenario.faction_count; ++index) {
      scenario.spawn_patterns[index] = pattern;
    }
  }
  if (auto it = root.find("spawn_patterns"); it != root.end()) {
    if (!it->is_object()) {
      fail("scenario field 'spawn_patterns' must be an object of faction "
           "keys");
    }
    for (auto entry = it->begin(); entry != it->end(); ++entry) {
      const auto faction = faction_from_key(entry.key());
      if (!faction ||
          static_cast<std::size_t>(*faction) >= scenario.faction_count) {
        fail("unknown spawn_patterns faction '" + entry.key() +
             "' (expected the scenario's army factions)");
      }
      scenario.spawn_patterns[static_cast<std::size_t>(*faction)] =
          parse_spawn_pattern(*entry,
                              "spawn_patterns." + entry.key());
    }
  }
  return scenario;
}

}  // namespace

Scenario parse_scenario(std::string_view json_text) {
  OrderedJson root;
  try {
    root = OrderedJson::parse(json_text.begin(), json_text.end());
  } catch (const nlohmann::json::exception& error) {
    fail(std::string("not valid JSON: ") + error.what());
  }
  return parse_scenario_document(root);
}

Scenario parse_scenario(std::istream& stream) {
  if (!stream.good()) {
    fail("scenario input stream is not readable");
  }
  OrderedJson root;
  try {
    root = OrderedJson::parse(stream);
  } catch (const nlohmann::json::exception& error) {
    fail(std::string("not valid JSON: ") + error.what());
  }
  return parse_scenario_document(root);
}

std::string write_scenario(const Scenario& scenario) {
  // ordered_json keeps army entries in scenario order — the parser
  // preserves file order, so the writer must too (round-trip).
  nlohmann::ordered_json out;
  out["battlefield"] = {{"width", scenario.battlefield_width},
                        {"height", scenario.battlefield_height}};
  out["spawn_band_fraction"] = scenario.spawn_band_fraction;
  nlohmann::ordered_json armies;
  for (std::size_t index = 0; index < scenario.faction_count; ++index) {
    nlohmann::ordered_json army = nlohmann::ordered_json::object();
    if (const auto& budget = scenario.budgets[index]) {
      army["budget"] = budget->budget;
      nlohmann::ordered_json mix = nlohmann::ordered_json::object();
      for (const ScenarioEntry& entry : budget->mix) {
        mix[entry.type_id] = entry.count;
      }
      army["mix"] = mix;
    } else {
      for (const ScenarioEntry& entry : scenario.armies[index]) {
        army[entry.type_id] = entry.count_max > 0
                                  ? nlohmann::json::array(
                                        {entry.count_min, entry.count_max})
                                  : nlohmann::json(entry.count);
      }
    }
    if (const auto& anchor = scenario.anchors[index]) {
      army["anchor"] = nlohmann::json::array({anchor->x, anchor->y});
    }
    armies[std::string(faction_key(static_cast<Faction>(index)))] =
        std::move(army);
  }
  out["armies"] = std::move(armies);
  bool any_objective = false;
  bool any_squad = false;
  for (std::size_t index = 0; index < scenario.faction_count; ++index) {
    any_objective = any_objective || scenario.objectives[index].has_value();
    any_squad = any_squad || scenario.squads[index].has_value();
  }
  if (any_objective) {
    nlohmann::ordered_json objectives;
    for (std::size_t index = 0; index < scenario.faction_count; ++index) {
      const auto& objective = scenario.objectives[index];
      if (objective) {
        objectives[std::string(faction_key(static_cast<Faction>(index)))] =
            {{"x", objective->x}, {"y", objective->y}};
      }
    }
    out["objectives"] = std::move(objectives);
  }
  if (any_squad) {
    nlohmann::ordered_json squads;
    for (std::size_t index = 0; index < scenario.faction_count; ++index) {
      const auto& plan = scenario.squads[index];
      if (plan) {
        squads[std::string(faction_key(static_cast<Faction>(index)))] = {
            {"count", plan->count}, {"size", plan->size}};
      }
    }
    out["squads"] = std::move(squads);
  }
  if (scenario.map) {
    out["map"] = {{"file", scenario.map->file},
                  {"cell_size", scenario.map->cell_size}};
  }
  bool same_pattern = true;
  for (std::size_t index = 1; index < scenario.faction_count; ++index) {
    same_pattern = same_pattern &&
                   scenario.spawn_patterns[index] ==
                       scenario.spawn_patterns[0];
  }
  if (same_pattern) {
    out["spawn_pattern"] = to_string(scenario.spawn_patterns[0]);
  } else {
    nlohmann::ordered_json patterns;
    for (std::size_t index = 0; index < scenario.faction_count; ++index) {
      patterns[std::string(faction_key(static_cast<Faction>(index)))] =
          to_string(scenario.spawn_patterns[index]);
    }
    out["spawn_patterns"] = std::move(patterns);
  }
  return out.dump(2) + "\n";
}

std::array<std::vector<TypeCount>, kMaxFactions> build_composition(
    const Scenario& scenario, const UnitTypeTable& types,
    std::uint64_t seed) {
  std::array<std::vector<TypeCount>, kMaxFactions> composition{};
  for (std::size_t index = 0; index < scenario.faction_count; ++index) {
    const Faction faction = static_cast<Faction>(index);
    // m13 item H(b): budget-form army. Deterministic largest-remainder
    // fill — every number below depends only on the scenario, the type
    // table and exact double arithmetic, never on the seed.
    if (const auto& spec = scenario.budgets[index]) {
      struct Fill {
        std::uint32_t type_index = 0;
        double cost = 1.0;
        double points = 0.0;  // this type's share of the budget
        std::uint32_t count = 0;
      };
      std::vector<Fill> fills;
      fills.reserve(spec->mix.size());
      double total_weight = 0.0;
      for (const ScenarioEntry& entry : spec->mix) {
        const int type_index = types.index_of(entry.type_id);
        if (type_index < 0) {
          fail("scenario army '" + std::string(to_string(faction)) +
               "' references unknown unit type '" + entry.type_id + "'");
        }
        Fill fill;
        fill.type_index = static_cast<std::uint32_t>(type_index);
        fill.cost = types.types[static_cast<std::size_t>(type_index)].cost;
        total_weight += static_cast<double>(entry.count);
        fills.push_back(fill);
      }
      double spent = 0.0;
      std::uint64_t total_units = 0;
      for (std::size_t i = 0; i < fills.size(); ++i) {
        const double weight = static_cast<double>(spec->mix[i].count);
        fills[i].points = spec->budget * weight / total_weight;
        fills[i].count =
            static_cast<std::uint32_t>(std::floor(fills[i].points /
                                                  fills[i].cost));
        if (fills[i].count == 0) {
          // Every mix type fields at least one unit; the min-one pass
          // below is what makes the leftover distribution start from a
          // legal army.
          fills[i].count = 1;
        }
        spent += static_cast<double>(fills[i].count) * fills[i].cost;
        total_units += fills[i].count;
      }
      if (spent > spec->budget) {
        fail("scenario army '" + std::string(to_string(faction)) +
             "': budget " + std::to_string(spec->budget) +
             " cannot field at least one unit of every mix type "
             "(needs >= " + std::to_string(spent) + " points)");
      }
      if (total_units > kMaxUnits) {
        fail("scenario army '" + std::string(to_string(faction)) +
             "': budget fill needs more than " + std::to_string(kMaxUnits) +
             " units, which exceeds the unit cap");
      }
      // Leftover points: one unit at a time to the largest affordable
      // fractional remainder (ties to the lower mix index). Terminates:
      // every round spends at least the cheapest cost in the mix.
      for (;;) {
        std::size_t best = fills.size();
        double best_gap = 0.0;
        double leftover = spec->budget - spent;
        for (std::size_t i = 0; i < fills.size(); ++i) {
          const double gap = fills[i].points -
                             static_cast<double>(fills[i].count) *
                                 fills[i].cost;
          if (gap > best_gap && fills[i].cost <= leftover) {
            best = i;
            best_gap = gap;
          }
        }
        if (best == fills.size()) {
          break;
        }
        ++fills[best].count;
        spent += fills[best].cost;
        ++total_units;
        if (total_units > kMaxUnits) {
          fail("scenario army '" + std::string(to_string(faction)) +
               "': budget fill needs more than " + std::to_string(kMaxUnits) +
               " units, which exceeds the unit cap");
        }
      }
      for (const Fill& fill : fills) {
        composition[index].push_back(
            TypeCount{fill.type_index, fill.count});
      }
      continue;
    }
    composition[index].reserve(scenario.armies[index].size());
    for (std::size_t entry_index = 0;
         entry_index < scenario.armies[index].size(); ++entry_index) {
      const ScenarioEntry& entry = scenario.armies[index][entry_index];
      const int type_index = types.index_of(entry.type_id);
      if (type_index < 0) {
        fail("scenario army '" + std::string(to_string(faction)) +
             "' references unknown unit type '" + entry.type_id + "'");
      }
      std::uint32_t count = entry.count;
      if (entry.count_max > 0) {
        // M10 range draw (counter RNG channel 4, keyed by seed, faction
        // and entry — order-independent and reproducible per seed).
        const std::uint32_t key =
            static_cast<std::uint32_t>((index << 16) | entry_index);
        const double roll = tactics_roll(seed, key, 0, 4);
        const std::uint32_t span = entry.count_max - entry.count_min + 1;
        count = entry.count_min +
                static_cast<std::uint32_t>(roll * span) % span;
      }
      composition[index].push_back(
          TypeCount{static_cast<std::uint32_t>(type_index), count});
    }
  }
  return composition;
}

}  // namespace sim
