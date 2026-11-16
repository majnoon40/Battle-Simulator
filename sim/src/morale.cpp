#include "sim/morale.hpp"

#include <algorithm>
#include <cmath>
#include <istream>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "sim/ai.hpp"

namespace sim {
namespace {

[[noreturn]] void fail(std::string message) {
  throw std::invalid_argument(std::move(message));
}

const nlohmann::json& required_field(const nlohmann::json& object,
                                     std::string_view section,
                                     std::string_view field) {
  const auto it = object.find(field);
  if (it == object.end()) {
    fail("morale config: '" + std::string(section) +
         "' is missing required field '" + std::string(field) + "'");
  }
  return *it;
}

// A finite number >= minimum (rates, thresholds, bounds).
double number_at_least(const nlohmann::json& value, double minimum,
                       std::string_view section, std::string_view field) {
  if (!value.is_number()) {
    fail("morale config: '" + std::string(section) + "." +
         std::string(field) + "' must be a number");
  }
  const double number = value.get<double>();
  if (!std::isfinite(number)) {
    fail("morale config: '" + std::string(section) + "." +
         std::string(field) + "' must be finite");
  }
  if (!(number >= minimum)) {
    fail("morale config: '" + std::string(section) + "." +
         std::string(field) + "' must be >= " + std::to_string(minimum));
  }
  return number;
}

double whole_number_at_least(const nlohmann::json& value,
                             std::uint64_t minimum, std::string_view section,
                             std::string_view field) {
  if (!value.is_number_integer()) {
    fail("morale config: '" + std::string(section) + "." +
         std::string(field) + "' must be a whole number");
  }
  const std::int64_t number = value.get<std::int64_t>();
  if (number < 0 ||
      static_cast<std::uint64_t>(number) < minimum) {
    fail("morale config: '" + std::string(section) + "." +
         std::string(field) + "' must be >= " + std::to_string(minimum));
  }
  return static_cast<double>(number);
}

// Parses one required object section and rejects unknown fields.
const nlohmann::json& section_of(const nlohmann::json& root,
                                 std::string_view name) {
  const nlohmann::json& section = required_field(root, "morale config", name);
  if (!section.is_object()) {
    fail("morale config: '" + std::string(name) + "' must be an object");
  }
  return section;
}

void reject_unknown(const nlohmann::json& object, std::string_view section,
                    std::initializer_list<std::string_view> known) {
  for (auto it = object.begin(); it != object.end(); ++it) {
    bool found = false;
    for (std::string_view field : known) {
      found = found || it.key() == field;
    }
    if (!found) {
      fail("morale config: unknown field '" + it.key() + "' in '" +
           std::string(section) + "'");
    }
  }
}

MoraleConfig parse_config(const nlohmann::json& root) {
  if (!root.is_object()) {
    fail("morale config must be a JSON object");
  }
  reject_unknown(root, "morale config",
                 {"start", "states", "loss", "gain", "thresholds", "recovery",
                  "ai", "max_loss_per_tick", "max_gain_per_tick",
                  "rout_timeout_ticks"});

  MoraleConfig config;

  {
    const nlohmann::json& start = section_of(root, "start");
    reject_unknown(start, "start", {"base", "bravery_scale"});
    config.start.base = number_at_least(required_field(start, "start", "base"),
                                        0.0, "start", "base");
    config.start.bravery_scale =
        number_at_least(required_field(start, "start", "bravery_scale"), 0.0,
                        "start", "bravery_scale");
    if (config.start.base + config.start.bravery_scale > 100.0) {
      fail("morale config: start.base + start.bravery_scale must be <= 100");
    }
  }

  {
    const nlohmann::json& states = section_of(root, "states");
    reject_unknown(states, "states",
                   {"confident_min", "steady_min", "worried_min", "shaken_min"});
    config.states.confident_min =
        number_at_least(required_field(states, "states", "confident_min"),
                        0.0, "states", "confident_min");
    config.states.steady_min =
        number_at_least(required_field(states, "states", "steady_min"), 0.0,
                        "states", "steady_min");
    config.states.worried_min =
        number_at_least(required_field(states, "states", "worried_min"), 0.0,
                        "states", "worried_min");
    config.states.shaken_min =
        number_at_least(required_field(states, "states", "shaken_min"), 0.0,
                        "states", "shaken_min");
    if (!(config.states.shaken_min < config.states.worried_min &&
          config.states.worried_min < config.states.steady_min &&
          config.states.steady_min < config.states.confident_min)) {
      fail("morale config: state thresholds must satisfy 0 <= shaken_min < "
           "worried_min < steady_min < confident_min");
    }
    if (config.states.confident_min > 100.0) {
      fail("morale config: confident_min must be <= 100");
    }
  }

  {
    const nlohmann::json& loss = section_of(root, "loss");
    reject_unknown(loss, "loss",
                   {"ally_death", "ally_rout", "routed_ally_presence_per_tick",
                    "outnumbered_per_tick", "damage_per_health_fraction",
                    "isolation_per_tick", "caution_amplifier",
                    "bravery_shield", "min_scale"});
    config.loss.ally_death =
        number_at_least(required_field(loss, "loss", "ally_death"), 0.0,
                        "loss", "ally_death");
    config.loss.ally_rout =
        number_at_least(required_field(loss, "loss", "ally_rout"), 0.0,
                        "loss", "ally_rout");
    config.loss.routed_ally_presence_per_tick = number_at_least(
        required_field(loss, "loss", "routed_ally_presence_per_tick"), 0.0,
        "loss", "routed_ally_presence_per_tick");
    config.loss.outnumbered_per_tick =
        number_at_least(required_field(loss, "loss", "outnumbered_per_tick"),
                        0.0, "loss", "outnumbered_per_tick");
    config.loss.damage_per_health_fraction = number_at_least(
        required_field(loss, "loss", "damage_per_health_fraction"), 0.0,
        "loss", "damage_per_health_fraction");
    config.loss.isolation_per_tick =
        number_at_least(required_field(loss, "loss", "isolation_per_tick"),
                        0.0, "loss", "isolation_per_tick");
    config.loss.caution_amplifier =
        number_at_least(required_field(loss, "loss", "caution_amplifier"), 0.0,
                        "loss", "caution_amplifier");
    config.loss.bravery_shield =
        number_at_least(required_field(loss, "loss", "bravery_shield"), 0.0,
                        "loss", "bravery_shield");
    config.loss.min_scale =
        number_at_least(required_field(loss, "loss", "min_scale"), 0.0,
                        "loss", "min_scale");
    if (config.loss.min_scale > 1.0) {
      fail("morale config: loss.min_scale must be <= 1");
    }
  }

  {
    const nlohmann::json& gain = section_of(root, "gain");
    reject_unknown(gain, "gain",
                   {"kill", "superiority_per_tick", "allies_present_per_tick",
                    "safe_per_tick"});
    config.gain.kill = number_at_least(required_field(gain, "gain", "kill"),
                                       0.0, "gain", "kill");
    config.gain.superiority_per_tick =
        number_at_least(required_field(gain, "gain", "superiority_per_tick"),
                        0.0, "gain", "superiority_per_tick");
    config.gain.allies_present_per_tick = number_at_least(
        required_field(gain, "gain", "allies_present_per_tick"), 0.0, "gain",
        "allies_present_per_tick");
    config.gain.safe_per_tick =
        number_at_least(required_field(gain, "gain", "safe_per_tick"), 0.0,
                        "gain", "safe_per_tick");
  }

  {
    const nlohmann::json& thresholds = section_of(root, "thresholds");
    reject_unknown(thresholds, "thresholds",
                   {"outnumbered_below", "superiority_above"});
    config.thresholds.outnumbered_below = number_at_least(
        required_field(thresholds, "thresholds", "outnumbered_below"), 0.0,
        "thresholds", "outnumbered_below");
    config.thresholds.superiority_above = number_at_least(
        required_field(thresholds, "thresholds", "superiority_above"), 0.0,
        "thresholds", "superiority_above");
    if (!(config.thresholds.outnumbered_below <
          config.thresholds.superiority_above)) {
      fail("morale config: thresholds.outnumbered_below must be < "
           "thresholds.superiority_above");
    }
    if (config.thresholds.superiority_above >= 1.0) {
      fail("morale config: thresholds.superiority_above must be < 1");
    }
  }

  {
    const nlohmann::json& recovery = section_of(root, "recovery");
    reject_unknown(recovery, "recovery", {"morale_threshold", "safe_ticks"});
    config.recovery.morale_threshold = number_at_least(
        required_field(recovery, "recovery", "morale_threshold"), 0.0,
        "recovery", "morale_threshold");
    config.recovery.safe_ticks = static_cast<std::uint64_t>(whole_number_at_least(
        required_field(recovery, "recovery", "safe_ticks"), 1, "recovery",
        "safe_ticks"));
    // Hysteresis requirement: recovery strictly above the breaking value.
    if (!(config.recovery.morale_threshold > config.states.shaken_min)) {
      fail("morale config: recovery.morale_threshold must be strictly greater "
           "than states.shaken_min (hysteresis: recover above the breaking "
           "value)");
    }
  }

  {
    const nlohmann::json& ai = section_of(root, "ai");
    reject_unknown(ai, "ai",
                   {"confident_bonus", "worried_penalty", "shaken_penalty"});
    config.ai.confident_bonus =
        number_at_least(required_field(ai, "ai", "confident_bonus"), 0.0,
                        "ai", "confident_bonus");
    config.ai.worried_penalty =
        number_at_least(required_field(ai, "ai", "worried_penalty"), 0.0, "ai",
                        "worried_penalty");
    config.ai.shaken_penalty =
        number_at_least(required_field(ai, "ai", "shaken_penalty"), 0.0, "ai",
                        "shaken_penalty");
    if (config.ai.confident_bonus > 1.0 || config.ai.worried_penalty > 1.0 ||
        config.ai.shaken_penalty > 1.0) {
      fail("morale config: ai modifiers must be <= 1");
    }
    if (!(config.ai.worried_penalty <= config.ai.shaken_penalty)) {
      fail("morale config: ai.worried_penalty must be <= ai.shaken_penalty "
           "(worse state, bigger penalty)");
    }
  }

  config.max_loss_per_tick = number_at_least(
      required_field(root, "morale config", "max_loss_per_tick"), 0.0,
      "morale config", "max_loss_per_tick");
  config.max_gain_per_tick = number_at_least(
      required_field(root, "morale config", "max_gain_per_tick"), 0.0,
      "morale config", "max_gain_per_tick");
  config.rout_timeout_ticks = static_cast<std::uint64_t>(
      whole_number_at_least(
          required_field(root, "morale config", "rout_timeout_ticks"), 1,
          "morale config", "rout_timeout_ticks"));

  return config;
}

}  // namespace

const char* to_string(MoraleState state) noexcept {
  switch (state) {
    case MoraleState::Confident:
      return "confident";
    case MoraleState::Steady:
      return "steady";
    case MoraleState::Worried:
      return "worried";
    case MoraleState::Shaken:
      return "shaken";
    case MoraleState::Broken:
      return "broken";
  }
  return "unknown";
}

MoraleConfig parse_morale(std::string_view json_text) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(json_text.begin(), json_text.end());
  } catch (const nlohmann::json::exception& error) {
    fail(std::string("not valid JSON: ") + error.what());
  }
  return parse_config(root);
}

MoraleConfig parse_morale(std::istream& stream) {
  if (!stream.good()) {
    fail("morale config input stream is not readable");
  }
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(stream);
  } catch (const nlohmann::json::exception& error) {
    fail(std::string("not valid JSON: ") + error.what());
  }
  return parse_config(root);
}

const MoraleConfig& default_morale_config() noexcept {
  static const MoraleConfig config = MoraleConfig{};
  return config;
}

MoraleState state_from_morale(double morale,
                              const MoraleConfig::States& states) noexcept {
  if (morale >= states.confident_min) {
    return MoraleState::Confident;
  }
  if (morale >= states.steady_min) {
    return MoraleState::Steady;
  }
  if (morale >= states.worried_min) {
    return MoraleState::Worried;
  }
  if (morale >= states.shaken_min) {
    return MoraleState::Shaken;
  }
  return MoraleState::Broken;
}

double starting_morale(double bravery,
                      const MoraleConfig::Start& start) noexcept {
  return std::clamp(start.base + start.bravery_scale * bravery, 0.0, 100.0);
}

double morale_delta(const MoraleInputs& inputs,
                    const MoraleConfig& config) noexcept {
  // Personality loss scaling: bravery shields, caution amplifies, and the
  // factor stays strictly positive so losses never become gains.
  const double scale = std::clamp(
      1.0 + config.loss.caution_amplifier * inputs.caution -
          config.loss.bravery_shield * inputs.bravery,
      config.loss.min_scale, 2.0);

  double delta = 0.0;
  // Losses (personality-scaled).
  delta -= config.loss.ally_death * inputs.ally_deaths * scale;
  delta -= config.loss.ally_rout * inputs.ally_routs * scale;
  delta -= config.loss.routed_ally_presence_per_tick * inputs.routed_allies *
           scale;
  delta -= config.loss.damage_per_health_fraction * inputs.damage_fraction *
           scale;
  if (inputs.enemies_visible) {
    if (inputs.strength_ratio < config.thresholds.outnumbered_below) {
      delta -= config.loss.outnumbered_per_tick * scale;
    }
    if (!inputs.allies_visible) {
      delta -= config.loss.isolation_per_tick * scale;
    }
  }
  // Gains (unscaled).
  delta += config.gain.kill * inputs.kills;
  if (inputs.enemies_visible) {
    if (inputs.strength_ratio > config.thresholds.superiority_above) {
      delta += config.gain.superiority_per_tick;
    }
    if (inputs.allies_visible) {
      delta += config.gain.allies_present_per_tick;
    }
  } else {
    delta += config.gain.safe_per_tick;
  }
  return std::clamp(delta, -config.max_loss_per_tick,
                    config.max_gain_per_tick);
}

void apply_morale_modifiers(ActionScores& scores, MoraleState state,
                            const MoraleConfig::Ai& ai) noexcept {
  const auto adjust = [&scores](double attack_shift, double retreat_shift) {
    const std::size_t attack = static_cast<std::size_t>(Action::Attack);
    const std::size_t approach = static_cast<std::size_t>(Action::Approach);
    const std::size_t retreat = static_cast<std::size_t>(Action::Retreat);
    if (scores[attack] >= 0.0) {
      scores[attack] = std::clamp(scores[attack] + attack_shift, 0.0, 1.0);
    }
    if (scores[approach] >= 0.0) {
      scores[approach] = std::clamp(scores[approach] + attack_shift, 0.0, 1.0);
    }
    if (scores[retreat] >= 0.0) {
      scores[retreat] = std::clamp(scores[retreat] + retreat_shift, 0.0, 1.0);
    }
  };
  switch (state) {
    case MoraleState::Confident:
      adjust(ai.confident_bonus, -ai.confident_bonus);
      break;
    case MoraleState::Steady:
      break;
    case MoraleState::Worried:
      adjust(-ai.worried_penalty, ai.worried_penalty);
      break;
    case MoraleState::Shaken:
      adjust(-ai.shaken_penalty, ai.shaken_penalty);
      break;
    case MoraleState::Broken:
      break;  // never scored: Broken units flee
  }
}

}  // namespace sim
