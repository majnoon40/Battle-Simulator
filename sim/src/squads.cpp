#include "sim/squads.hpp"

#include <algorithm>
#include <cmath>
#include <istream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

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

// A whole number of ticks >= min_ticks.
std::uint64_t tick_count(const nlohmann::json& value,
                         const std::string& owner, std::string_view field,
                         std::uint64_t min_ticks) {
  if (!value.is_number_integer()) {
    fail(owner + ": field '" + std::string(field) +
         "' must be a whole number of ticks");
  }
  if (value.is_number_unsigned()) {
    const std::uint64_t ticks = value.get<std::uint64_t>();
    if (ticks < min_ticks) {
      fail(owner + ": field '" + std::string(field) + "' must be >= " +
           std::to_string(min_ticks));
    }
    return ticks;
  }
  const std::int64_t ticks = value.get<std::int64_t>();
  if (ticks < 0 || static_cast<std::uint64_t>(ticks) < min_ticks) {
    fail(owner + ": field '" + std::string(field) + "' must be >= " +
         std::to_string(min_ticks));
  }
  return static_cast<std::uint64_t>(ticks);
}

FormationKind formation_kind_of(std::string_view id) {
  if (id == "line") {
    return FormationKind::Line;
  }
  if (id == "column") {
    return FormationKind::Column;
  }
  if (id == "loose") {
    return FormationKind::Loose;
  }
  fail("unknown formation '" + std::string(id) +
       "' (expected line, column or loose)");
}

// Center-out index within a rank: 0 stays in the middle, then +1 to the
// right, -1 to the left, +2 right, -2 left, ...
double center_out_offset(std::size_t index) noexcept {
  if (index == 0) {
    return 0.0;
  }
  const double magnitude = static_cast<double>((index + 1) / 2);
  return index % 2 == 1 ? magnitude : -magnitude;
}

}  // namespace

const char* to_string(Order order) noexcept {
  switch (order) {
    case Order::Advance:
      return "advance";
    case Order::Hold:
      return "hold";
    case Order::Regroup:
      return "regroup";
    case Order::Retreat:
      return "retreat";
    case Order::None:
      return "none";
  }
  return "none";
}

FormationConfig parse_formations(std::string_view json_text) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(json_text.begin(), json_text.end());
  } catch (const nlohmann::json::parse_error& error) {
    fail(std::string("not valid JSON: ") + error.what());
  }
  if (!root.is_object()) {
    fail("formation config must be a JSON object");
  }
  for (auto it = root.begin(); it != root.end(); ++it) {
    if (it.key() != "formations" && it.key() != "default_formation" &&
        it.key() != "steering") {
      fail("unknown field '" + it.key() +
           "' in formation config (expected formations, default_formation, "
           "steering)");
    }
  }

  FormationConfig config;
  const nlohmann::json& formations =
      required_field(root, "formation config", "formations");
  if (!formations.is_object() || formations.empty()) {
    fail("formation config: 'formations' must be a non-empty object");
  }
  for (auto it = formations.begin(); it != formations.end(); ++it) {
    const std::string owner = "formation '" + it.key() + "'";
    if (!it->is_object()) {
      fail(owner + " must be an object");
    }
    for (auto field = it->begin(); field != it->end(); ++field) {
      if (field.key() != "spacing") {
        fail(owner + ": unknown field '" + field.key() +
             "' (expected spacing)");
      }
    }
    FormationConfig::Definition definition;
    definition.id = it.key();
    definition.kind = formation_kind_of(it.key());
    definition.spacing = finite_number(
        required_field(*it, owner, "spacing"), owner, "spacing");
    if (!(definition.spacing > 0.0)) {
      fail(owner + ": spacing must be > 0");
    }
    if (std::find_if(config.formations.begin(), config.formations.end(),
                     [&](const FormationConfig::Definition& existing) {
                       return existing.id == definition.id;
                     }) != config.formations.end()) {
      fail("duplicate formation id '" + definition.id + "'");
    }
    config.formations.push_back(std::move(definition));
  }

  const nlohmann::json& default_name =
      required_field(root, "formation config", "default_formation");
  if (!default_name.is_string()) {
    fail("formation config: 'default_formation' must be a string");
  }
  config.default_formation = default_name.get<std::string>();
  const auto found = std::find_if(
      config.formations.begin(), config.formations.end(),
      [&](const FormationConfig::Definition& definition) {
        return definition.id == config.default_formation;
      });
  if (found == config.formations.end()) {
    fail("default_formation '" + config.default_formation +
         "' is not a defined formation");
  }

  const nlohmann::json& steering =
      required_field(root, "formation config", "steering");
  if (!steering.is_object()) {
    fail("formation config: 'steering' must be an object");
  }
  for (auto it = steering.begin(); it != steering.end(); ++it) {
    if (it.key() != "separation" && it.key() != "cohesion" &&
        it.key() != "alignment" && it.key() != "slot_seeking" &&
        it.key() != "separation_radius" && it.key() != "cohesion_reference") {
      fail("formation config: unknown steering field '" + it.key() + "'");
    }
  }
  const std::string owner = "formation config steering";
  config.steering.separation = finite_number(
      required_field(steering, owner, "separation"), owner, "separation");
  config.steering.cohesion = finite_number(
      required_field(steering, owner, "cohesion"), owner, "cohesion");
  config.steering.alignment = finite_number(
      required_field(steering, owner, "alignment"), owner, "alignment");
  config.steering.slot_seeking = finite_number(
      required_field(steering, owner, "slot_seeking"), owner, "slot_seeking");
  config.steering.separation_radius =
      finite_number(required_field(steering, owner, "separation_radius"), owner,
                    "separation_radius");
  config.steering.cohesion_reference = finite_number(
      required_field(steering, owner, "cohesion_reference"), owner,
      "cohesion_reference");
  if (!(config.steering.separation >= 0.0) ||
      !(config.steering.cohesion >= 0.0) ||
      !(config.steering.alignment >= 0.0) ||
      !(config.steering.slot_seeking >= 0.0)) {
    fail(owner + ": weights must be >= 0");
  }
  if (!(config.steering.separation_radius > 0.0)) {
    fail(owner + ": separation_radius must be > 0");
  }
  if (!(config.steering.cohesion_reference > 0.0)) {
    fail(owner + ": cohesion_reference must be > 0");
  }
  return config;
}

FormationConfig parse_formations(std::istream& stream) {
  if (!stream.good()) {
    fail("formation config input stream is not readable");
  }
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(stream);
  } catch (const nlohmann::json::parse_error& error) {
    fail(std::string("not valid JSON: ") + error.what());
  }
  // Reuse the text parser for everything after the JSON decode.
  return parse_formations(root.dump());
}

const FormationConfig& default_formation_config() noexcept {
  static const FormationConfig config = [] {
    FormationConfig config;
    auto add = [&](const char* id, FormationKind kind, double spacing) {
      config.formations.push_back(
          FormationConfig::Definition{id, kind, spacing});
    };
    // Alphabetical order, matching the shipped data/formations.json as
    // nlohmann::json iterates object keys (column, line, loose).
    add("column", FormationKind::Column, 4.0);
    add("line", FormationKind::Line, 3.0);
    add("loose", FormationKind::Loose, 6.0);
    config.default_formation = "line";
    return config;
  }();
  return config;
}

CommanderConfig parse_commanders(std::string_view json_text) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(json_text.begin(), json_text.end());
  } catch (const nlohmann::json::parse_error& error) {
    fail(std::string("not valid JSON: ") + error.what());
  }
  if (!root.is_object()) {
    fail("commander config must be a JSON object");
  }
  for (auto it = root.begin(); it != root.end(); ++it) {
    if (it.key() != "orders" && it.key() != "order_weights" &&
        it.key() != "order_min_duration_ticks" &&
        it.key() != "order_delay_per_unit" && it.key() != "order_decay_ticks" &&
        it.key() != "succession_delay_ticks" &&
        it.key() != "commander_death_morale_loss" && it.key() != "obedience") {
      fail("unknown field '" + it.key() +
           "' in commander config (expected orders, order_weights, "
           "order_min_duration_ticks, order_delay_per_unit, "
           "order_decay_ticks, succession_delay_ticks, "
           "commander_death_morale_loss, obedience)");
    }
  }

  CommanderConfig config;
  const nlohmann::json& orders = required_field(root, "commander config", "orders");
  if (!orders.is_object()) {
    fail("commander config: 'orders' must be an object");
  }
  for (auto it = orders.begin(); it != orders.end(); ++it) {
    if (it.key() != "retreat" && it.key() != "regroup" && it.key() != "hold") {
      fail("commander config orders: unknown order '" + it.key() +
           "' (expected retreat, regroup, hold; advance is the default)");
    }
    if (!it->is_object()) {
      fail("commander config orders '" + it.key() + "' must be an object");
    }
  }
  const nlohmann::json& retreat = required_field(orders, "commander config orders", "retreat");
  for (auto it = retreat.begin(); it != retreat.end(); ++it) {
    if (it.key() != "cohesion_below" && it.key() != "strength_below" &&
        it.key() != "morale_below") {
      fail("commander config orders retreat: unknown field '" + it.key() + "'");
    }
  }
  const std::string retreat_owner = "commander config orders retreat";
  config.rules.retreat_cohesion_below = finite_number(
      required_field(retreat, retreat_owner, "cohesion_below"), retreat_owner,
      "cohesion_below");
  config.rules.retreat_strength_below = finite_number(
      required_field(retreat, retreat_owner, "strength_below"), retreat_owner,
      "strength_below");
  config.rules.retreat_morale_below = finite_number(
      required_field(retreat, retreat_owner, "morale_below"), retreat_owner,
      "morale_below");
  const nlohmann::json& regroup = required_field(orders, "commander config orders", "regroup");
  for (auto it = regroup.begin(); it != regroup.end(); ++it) {
    if (it.key() != "cohesion_below") {
      fail("commander config orders regroup: unknown field '" + it.key() +
           "' (expected cohesion_below)");
    }
  }
  config.rules.regroup_cohesion_below = finite_number(
      required_field(regroup, "commander config orders regroup", "cohesion_below"),
      "commander config orders regroup", "cohesion_below");
  const nlohmann::json& hold = required_field(orders, "commander config orders", "hold");
  for (auto it = hold.begin(); it != hold.end(); ++it) {
    if (it.key() != "strength_above") {
      fail("commander config orders hold: unknown field '" + it.key() +
           "' (expected strength_above)");
    }
  }
  config.rules.hold_strength_above = finite_number(
      required_field(hold, "commander config orders hold", "strength_above"),
      "commander config orders hold", "strength_above");
  if (!(config.rules.retreat_cohesion_below >= 0.0) ||
      !(config.rules.retreat_cohesion_below <= 1.0)) {
    fail("commander config orders retreat: cohesion_below must be in [0, 1]");
  }
  if (!(config.rules.regroup_cohesion_below >= 0.0) ||
      !(config.rules.regroup_cohesion_below <= 1.0)) {
    fail("commander config orders regroup: cohesion_below must be in [0, 1]");
  }
  if (!(config.rules.retreat_strength_below >= 0.0) ||
      !(config.rules.retreat_strength_below <= 1.0)) {
    fail("commander config orders retreat: strength_below must be in [0, 1]");
  }
  if (!(config.rules.hold_strength_above >= 0.0) ||
      !(config.rules.hold_strength_above <= 1.0)) {
    fail("commander config orders hold: strength_above must be in [0, 1]");
  }
  if (!(config.rules.retreat_morale_below >= 0.0) ||
      !(config.rules.retreat_morale_below <= 100.0)) {
    fail("commander config orders retreat: morale_below must be in [0, 100]");
  }

  const nlohmann::json& weights =
      required_field(root, "commander config", "order_weights");
  if (!weights.is_object()) {
    fail("commander config: 'order_weights' must be an object");
  }
  for (auto it = weights.begin(); it != weights.end(); ++it) {
    if (it.key() != "advance" && it.key() != "hold" && it.key() != "regroup" &&
        it.key() != "retreat") {
      fail("commander config order_weights: unknown order '" + it.key() + "'");
    }
  }
  const std::string weight_owner = "commander config order_weights";
  config.order_weights.advance = finite_number(
      required_field(weights, weight_owner, "advance"), weight_owner, "advance");
  config.order_weights.hold = finite_number(
      required_field(weights, weight_owner, "hold"), weight_owner, "hold");
  config.order_weights.regroup = finite_number(
      required_field(weights, weight_owner, "regroup"), weight_owner, "regroup");
  config.order_weights.retreat = finite_number(
      required_field(weights, weight_owner, "retreat"), weight_owner, "retreat");
  for (const double weight : {config.order_weights.advance,
                              config.order_weights.hold,
                              config.order_weights.regroup,
                              config.order_weights.retreat}) {
    if (!(weight >= 0.0) || !(weight <= 1.0)) {
      fail(weight_owner + ": order weights must be in [0, 1] (bounded terms)");
    }
  }

  config.order_min_duration_ticks = tick_count(
      required_field(root, "commander config", "order_min_duration_ticks"),
      "commander config", "order_min_duration_ticks", 1);
  config.order_decay_ticks = tick_count(
      required_field(root, "commander config", "order_decay_ticks"),
      "commander config", "order_decay_ticks", 1);
  config.succession_delay_ticks = tick_count(
      required_field(root, "commander config", "succession_delay_ticks"),
      "commander config", "succession_delay_ticks", 1);
  config.order_delay_per_unit = finite_number(
      required_field(root, "commander config", "order_delay_per_unit"),
      "commander config", "order_delay_per_unit");
  if (!(config.order_delay_per_unit >= 0.0)) {
    fail("commander config: order_delay_per_unit must be >= 0");
  }
  config.commander_death_morale_loss = finite_number(
      required_field(root, "commander config", "commander_death_morale_loss"),
      "commander config", "commander_death_morale_loss");
  if (!(config.commander_death_morale_loss >= 0.0)) {
    fail("commander config: commander_death_morale_loss must be >= 0");
  }

  const nlohmann::json& obedience =
      required_field(root, "commander config", "obedience");
  if (!obedience.is_object()) {
    fail("commander config: 'obedience' must be an object");
  }
  for (auto it = obedience.begin(); it != obedience.end(); ++it) {
    if (it.key() != "morale_factor" && it.key() != "cohesion_floor") {
      fail("commander config obedience: unknown field '" + it.key() +
           "' (expected morale_factor, cohesion_floor)");
    }
  }
  const nlohmann::json& morale_factor =
      required_field(obedience, "commander config obedience", "morale_factor");
  if (!morale_factor.is_object()) {
    fail("commander config obedience: 'morale_factor' must be an object");
  }
  for (auto it = morale_factor.begin(); it != morale_factor.end(); ++it) {
    if (it.key() != "confident" && it.key() != "steady" &&
        it.key() != "worried" && it.key() != "shaken") {
      fail("commander config obedience morale_factor: unknown state '" +
           it.key() + "'");
    }
  }
  const std::string factor_owner = "commander config obedience morale_factor";
  config.obedience.morale_factor_confident =
      finite_number(required_field(morale_factor, factor_owner, "confident"),
                    factor_owner, "confident");
  config.obedience.morale_factor_steady = finite_number(
      required_field(morale_factor, factor_owner, "steady"), factor_owner,
      "steady");
  config.obedience.morale_factor_worried = finite_number(
      required_field(morale_factor, factor_owner, "worried"), factor_owner,
      "worried");
  config.obedience.morale_factor_shaken = finite_number(
      required_field(morale_factor, factor_owner, "shaken"), factor_owner,
      "shaken");
  for (const double factor :
       {config.obedience.morale_factor_confident,
        config.obedience.morale_factor_steady,
        config.obedience.morale_factor_worried,
        config.obedience.morale_factor_shaken}) {
    if (!(factor >= 0.0) || !(factor <= 1.5)) {
      fail(factor_owner + ": state factors must be in [0, 1.5]");
    }
  }
  config.obedience.cohesion_floor = finite_number(
      required_field(obedience, "commander config obedience", "cohesion_floor"),
      "commander config obedience", "cohesion_floor");
  if (!(config.obedience.cohesion_floor >= 0.0) ||
      !(config.obedience.cohesion_floor <= 1.0)) {
    fail("commander config obedience: cohesion_floor must be in [0, 1]");
  }
  return config;
}

CommanderConfig parse_commanders(std::istream& stream) {
  if (!stream.good()) {
    fail("commander config input stream is not readable");
  }
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(stream);
  } catch (const nlohmann::json::parse_error& error) {
    fail(std::string("not valid JSON: ") + error.what());
  }
  return parse_commanders(root.dump());
}

const CommanderConfig& default_commander_config() noexcept {
  static const CommanderConfig config;
  return config;
}

// Center-out placement is defined above (center_out_offset).

SlotOffset formation_slot(FormationKind kind, std::size_t member_count,
                          double spacing, std::size_t index) noexcept {
  // Defensive clamping: callers pass in-range indices (validated squads);
  // out-of-range indices reuse the nearest slot shape position.
  if (member_count == 0) {
    return SlotOffset{};
  }
  index = std::min(index, member_count - 1);
  SlotOffset slot;
  switch (kind) {
    case FormationKind::Line:
      // One rank along the lateral axis, filled center-out; the commander
      // (member 0) holds the center.
      slot.lateral = center_out_offset(index) * spacing;
      break;
    case FormationKind::Column:
      // Single file along the forward axis; the commander leads at the
      // front and the squad trails behind it.
      slot.forward = -static_cast<double>(index) * spacing;
      break;
    case FormationKind::Loose: {
      // A square-ish block: rows from the front, each row filled center-out.
      const std::size_t width =
          std::max<std::size_t>(1, static_cast<std::size_t>(
                                       std::ceil(std::sqrt(
                                           static_cast<double>(member_count)))));
      const std::size_t row = index / width;
      const std::size_t column = index % width;
      slot.forward = static_cast<double>(row) * spacing;
      slot.lateral = center_out_offset(column) * spacing;
      break;
    }
  }
  return slot;
}

Order pick_order(double cohesion, double strength, double squad_morale,
                 bool enemies_visible,
                 const CommanderConfig::Rules& rules) noexcept {
  if (cohesion < rules.retreat_cohesion_below ||
      strength < rules.retreat_strength_below ||
      squad_morale < rules.retreat_morale_below) {
    return Order::Retreat;
  }
  if (cohesion < rules.regroup_cohesion_below) {
    return Order::Regroup;
  }
  if (enemies_visible && strength > rules.hold_strength_above) {
    return Order::Hold;
  }
  return Order::Advance;
}

double obedience_weight(double discipline, double distance,
                        double leadership_radius, MoraleState state,
                        double squad_cohesion,
                        const CommanderConfig& config) noexcept {
  if (!(leadership_radius > 0.0)) {
    return 0.0;  // no commander, no obedience
  }
  const double distance_factor =
      std::clamp(1.0 - distance / leadership_radius, 0.0, 1.0);
  double state_factor = config.obedience.morale_factor_steady;
  switch (state) {
    case MoraleState::Confident:
      state_factor = config.obedience.morale_factor_confident;
      break;
    case MoraleState::Steady:
      break;
    case MoraleState::Worried:
      state_factor = config.obedience.morale_factor_worried;
      break;
    case MoraleState::Shaken:
      state_factor = config.obedience.morale_factor_shaken;
      break;
    case MoraleState::Broken:
      state_factor = 0.0;  // routed units never score and never obey
      break;
  }
  const double cohesion_factor =
      config.obedience.cohesion_floor +
      (1.0 - config.obedience.cohesion_floor) * std::clamp(squad_cohesion, 0.0, 1.0);
  return std::clamp(std::clamp(discipline, 0.0, 1.0) * distance_factor *
                       state_factor * cohesion_factor,
                    0.0, 1.0);
}

std::array<double, kActionCount> order_score_bonus(
    Order order, double obedience,
    const CommanderConfig::OrderWeights& weights) noexcept {
  std::array<double, kActionCount> bonus{};
  const double term = std::clamp(obedience, 0.0, 1.0);
  switch (order) {
    case Order::Advance:
      bonus[static_cast<std::size_t>(Action::Advance)] = term * weights.advance;
      bonus[static_cast<std::size_t>(Action::Approach)] = term * weights.advance;
      // M10: an advancing member uses shared intel to push toward the
      // enemy — Search carries the same bonus (it only ever competes with
      // tactics on; the M6 action set is untouched).
      bonus[static_cast<std::size_t>(Action::Search)] = term * weights.advance;
      break;
    case Order::Hold:
      bonus[static_cast<std::size_t>(Action::Hold)] = term * weights.hold;
      bonus[static_cast<std::size_t>(Action::Attack)] = term * weights.hold;
      break;
    case Order::Regroup:
      bonus[static_cast<std::size_t>(Action::Hold)] = term * weights.regroup;
      break;
    case Order::Retreat:
      bonus[static_cast<std::size_t>(Action::Retreat)] = term * weights.retreat;
      break;
    case Order::None:
      break;
  }
  return bonus;
}

}  // namespace sim
