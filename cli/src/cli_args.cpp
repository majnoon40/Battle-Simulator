#include "cli/cli_args.hpp"
#include <filesystem>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string>

namespace cli {
namespace {

bool is_unsigned_digits(const std::string& text) {
  if (text.empty()) {
    return false;
  }
  for (char c : text) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

struct ParsedNumber {
  bool ok = false;
  unsigned long long value = 0;
  std::string error;
};

// Accepts only plain digit strings: signs, whitespace, decimals and other
// strtoull-isms are rejected so that negative or malformed values produce a
// clear error instead of silently wrapping around.
ParsedNumber parse_unsigned(const std::string& text, const char* name,
                            unsigned long long max) {
  ParsedNumber parsed;
  if (!is_unsigned_digits(text)) {
    parsed.error =
        std::string(name) + " expects a non-negative integer, got '" + text + "'";
    return parsed;
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  if (errno == ERANGE || value > max) {
    parsed.error = std::string(name) + " is out of range (max " +
                   std::to_string(max) + ")";
    return parsed;
  }
  parsed.ok = true;
  parsed.value = value;
  return parsed;
}

}  // namespace

// Strict positive decimal (digits with an optional fractional part); signs,
// exponents and whitespace are rejected so bad values produce clear errors.
bool parse_positive_double(const std::string& text, double& out) {
  if (text.empty()) {
    return false;
  }
  std::size_t digits_before_dot = 0;
  std::size_t i = 0;
  while (i < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[i]))) {
    ++i;
    ++digits_before_dot;
  }
  std::size_t digits_after_dot = 0;
  if (i < text.size() && text[i] == '.') {
    ++i;
    while (i < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[i]))) {
      ++i;
      ++digits_after_dot;
    }
  }
  if (i != text.size() || digits_before_dot == 0 ||
      (text.find('.') != std::string::npos && digits_after_dot == 0)) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (errno == ERANGE || end == nullptr || *end != '\0' || !(value > 0.0)) {
    return false;
  }
  out = value;
  return true;
}

ParseOutcome parse_args(int argc, char** argv) {
  ParseOutcome outcome;
  outcome.ok = true;
  Arguments& args = outcome.args;

  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];

    // Consume the argument after the current flag, or fail with a clear
    // message when it is missing.
    auto require_value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        outcome.ok = false;
        outcome.error = std::string(name) + " requires a value";
        return nullptr;
      }
      return argv[++i];
    };

    if (flag == "--help" || flag == "-h") {
      args.help = true;
      return outcome;
    }
    if (flag == "--seed") {
      const char* value = require_value("--seed");
      if (value == nullptr) {
        return outcome;
      }
      const ParsedNumber parsed =
          parse_unsigned(value, "--seed", std::numeric_limits<std::uint64_t>::max());
      if (!parsed.ok) {
        outcome.ok = false;
        outcome.error = parsed.error;
        return outcome;
      }
      args.seed = static_cast<std::uint64_t>(parsed.value);
      continue;
    }
    if (flag == "--units") {
      const char* value = require_value("--units");
      if (value == nullptr) {
        return outcome;
      }
      const ParsedNumber parsed =
          parse_unsigned(value, "--units", std::numeric_limits<std::uint32_t>::max());
      if (!parsed.ok) {
        outcome.ok = false;
        outcome.error = parsed.error;
        return outcome;
      }
      args.units = static_cast<std::uint32_t>(parsed.value);
      args.units_explicit = true;
      continue;
    }
    if (flag == "--config") {
      const char* value = require_value("--config");
      if (value == nullptr) {
        return outcome;
      }
      if (*value == '\0') {
        outcome.ok = false;
        outcome.error = "--config requires a non-empty file path";
        return outcome;
      }
      args.config_path = value;
      continue;
    }
    if (flag == "--scenario") {
      const char* value = require_value("--scenario");
      if (value == nullptr) {
        return outcome;
      }
      if (*value == '\0') {
        outcome.ok = false;
        outcome.error = "--scenario requires a non-empty file path";
        return outcome;
      }
      args.scenario_path = value;
      continue;
    }
    if (flag == "--ticks") {
      const char* value = require_value("--ticks");
      if (value == nullptr) {
        return outcome;
      }
      const ParsedNumber parsed =
          parse_unsigned(value, "--ticks", std::numeric_limits<std::uint64_t>::max());
      if (!parsed.ok) {
        outcome.ok = false;
        outcome.error = parsed.error;
        return outcome;
      }
      args.ticks = static_cast<std::uint64_t>(parsed.value);
      continue;
    }
    if (flag == "--neighbor-mode") {
      const char* value = require_value("--neighbor-mode");
      if (value == nullptr) {
        return outcome;
      }
      const std::string mode = value;
      if (mode != "brute" && mode != "grid") {
        outcome.ok = false;
        outcome.error =
            "--neighbor-mode expects 'brute' or 'grid', got '" + mode + "'";
        return outcome;
      }
      args.grid_mode = mode == "grid";
      args.neighbor_mode_explicit = true;
      continue;
    }
    if (flag == "--grid-cell") {
      const char* value = require_value("--grid-cell");
      if (value == nullptr) {
        return outcome;
      }
      double cell = 0.0;
      if (!parse_positive_double(value, cell)) {
        outcome.ok = false;
        outcome.error =
            "--grid-cell expects a positive number of world units, got '" +
            std::string(value) + "'";
        return outcome;
      }
      args.grid_cell_size = cell;
      args.grid_cell_explicit = true;
      continue;
    }
    if (flag == "--ai-mode") {
      const char* value = require_value("--ai-mode");
      if (value == nullptr) {
        return outcome;
      }
      const std::string mode = value;
      if (mode != "legacy" && mode != "utility") {
        outcome.ok = false;
        outcome.error =
            "--ai-mode expects 'legacy' or 'utility', got '" + mode + "'";
        return outcome;
      }
      args.utility_ai = mode == "utility";
      args.ai_mode_explicit = true;
      continue;
    }
    if (flag == "--personalities") {
      const char* value = require_value("--personalities");
      if (value == nullptr) {
        return outcome;
      }
      if (*value == '\0') {
        outcome.ok = false;
        outcome.error = "--personalities requires a non-empty file path";
        return outcome;
      }
      args.personalities_path = value;
      continue;
    }
    if (flag == "--ai-debug") {
      const char* value = require_value("--ai-debug");
      if (value == nullptr) {
        return outcome;
      }
      const ParsedNumber parsed =
          parse_unsigned(value, "--ai-debug", 0xFFFFFFFFULL);
      if (!parsed.ok) {
        outcome.ok = false;
        outcome.error = parsed.error;
        return outcome;
      }
      if (parsed.value == 0) {
        outcome.ok = false;
        outcome.error = "--ai-debug expects a unit id >= 1";
        return outcome;
      }
      args.ai_debug_unit = static_cast<std::uint32_t>(parsed.value);
      continue;
    }
    if (flag == "--ai-debug-tick") {
      const char* value = require_value("--ai-debug-tick");
      if (value == nullptr) {
        return outcome;
      }
      const ParsedNumber parsed =
          parse_unsigned(value, "--ai-debug-tick", 0xFFFFFFFFFFFFFFFFULL);
      if (!parsed.ok) {
        outcome.ok = false;
        outcome.error = parsed.error;
        return outcome;
      }
      if (parsed.value == 0) {
        outcome.ok = false;
        outcome.error = "--ai-debug-tick expects a tick number >= 1";
        return outcome;
      }
      args.ai_debug_tick = parsed.value;
      args.ai_debug_tick_explicit = true;
      continue;
    }
    if (flag == "--morale") {
      const char* value = require_value("--morale");
      if (value == nullptr) {
        return outcome;
      }
      const std::string mode = value;
      if (mode != "on" && mode != "off") {
        outcome.ok = false;
        outcome.error =
            "--morale expects 'on' or 'off', got '" + mode + "'";
        return outcome;
      }
      args.morale_on = mode == "on";
      args.morale_explicit = true;
      continue;
    }
    if (flag == "--morale-config") {
      const char* value = require_value("--morale-config");
      if (value == nullptr) {
        return outcome;
      }
      if (*value == '\0') {
        outcome.ok = false;
        outcome.error = "--morale-config requires a non-empty file path";
        return outcome;
      }
      args.morale_config_path = value;
      continue;
    }
    if (flag == "--squads") {
      const char* value = require_value("--squads");
      if (value == nullptr) {
        return outcome;
      }
      const std::string mode = value;
      if (mode != "on" && mode != "off") {
        outcome.ok = false;
        outcome.error = "--squads expects 'on' or 'off', got '" + mode + "'";
        return outcome;
      }
      args.squads_on = mode == "on";
      args.squads_explicit = true;
      continue;
    }
    if (flag == "--variance") {
      const char* value = require_value("--variance");
      if (value == nullptr) {
        return outcome;
      }
      const std::string mode = value;
      if (mode != "on" && mode != "off") {
        outcome.ok = false;
        outcome.error = "--variance expects 'on' or 'off', got '" + mode + "'";
        return outcome;
      }
      args.variance_on = mode == "on";
      args.variance_explicit = true;
      continue;
    }
    if (flag == "--events") {
      const char* value = require_value("--events");
      if (value == nullptr) {
        return outcome;
      }
      args.events_on = true;
      args.events_path = value;
      continue;
    }
    if (flag == "--trace") {
      const char* value = require_value("--trace");
      if (value == nullptr) {
        return outcome;
      }
      args.trace_on = true;
      args.trace_path = value;
      continue;
    }
    if (flag == "--trace-interval") {
      const char* value = require_value("--trace-interval");
      if (value == nullptr) {
        return outcome;
      }
      const ParsedNumber parsed =
          parse_unsigned(value, "--trace-interval", 100000);
      if (!parsed.ok || parsed.value < 1) {
        outcome.ok = false;
        outcome.error = parsed.ok
                            ? "--trace-interval must be in [1, 100000]"
                            : parsed.error;
        return outcome;
      }
      args.trace_interval = static_cast<std::uint32_t>(parsed.value);
      continue;
    }
    if (flag == "--export-csv") {
      const char* value = require_value("--export-csv");
      if (value == nullptr) {
        return outcome;
      }
      args.export_csv = true;
      args.csv_path = value;
      continue;
    }
    if (flag == "--csv-interval") {
      const char* value = require_value("--csv-interval");
      if (value == nullptr) {
        return outcome;
      }
      const ParsedNumber parsed =
          parse_unsigned(value, "--csv-interval", 100000);
      if (!parsed.ok || parsed.value < 1) {
        outcome.ok = false;
        outcome.error = parsed.ok
                            ? "--csv-interval must be in [1, 100000]"
                            : parsed.error;
        return outcome;
      }
      args.csv_interval = static_cast<std::uint32_t>(parsed.value);
      continue;
    }
    if (flag == "--report") {
      const char* value = require_value("--report");
      if (value == nullptr) {
        return outcome;
      }
      args.report_on = true;
      args.report_path = value;
      continue;
    }
    if (flag == "--report-png") {
      const char* value = require_value("--report-png");
      if (value == nullptr) {
        return outcome;
      }
      args.report_png_on = true;
      args.report_png_path = value;
      continue;
    }
    if (flag == "--record") {
      const char* value = require_value("--record");
      if (value == nullptr) {
        return outcome;
      }
      args.record_on = true;
      args.record_path = value;
      continue;
    }
    if (flag == "--replay") {
      const char* value = require_value("--replay");
      if (value == nullptr) {
        return outcome;
      }
      args.replay_on = true;
      args.replay_path = value;
      continue;
    }
    if (flag == "--verify") {
      args.replay_verify = true;
      continue;
    }
    if (flag == "--seek") {
      const char* value = require_value("--seek");
      if (value == nullptr) {
        return outcome;
      }
      const ParsedNumber parsed = parse_unsigned(
          value, "--seek", std::numeric_limits<std::uint64_t>::max());
      if (!parsed.ok || parsed.value == 0) {
        outcome.ok = false;
        outcome.error = parsed.ok ? "--seek expects a tick >= 1"
                                  : parsed.error;
        return outcome;
      }
      args.replay_seek = parsed.value;
      continue;
    }
    if (flag == "--intervene") {
      const char* value = require_value("--intervene");
      if (value == nullptr) {
        return outcome;
      }
      // Format: kill:UNIT:TICK (the unit is dead by the end of TICK).
      std::string spec = value;
      if (spec.rfind("kill:", 0) != 0) {
        outcome.ok = false;
        outcome.error =
            "--intervene expects kill:UNIT:TICK, got '" + spec + "'";
        return outcome;
      }
      const std::size_t first = spec.find(':', 5);
      if (first == std::string::npos) {
        outcome.ok = false;
        outcome.error = "--intervene expects kill:UNIT:TICK";
        return outcome;
      }
      const std::string unit_text = spec.substr(5, first - 5);
      const std::string tick_text = spec.substr(first + 1);
      // Strict unsigned parsing for both parts: trailing characters,
      // signs, empty fields and out-of-range values are all rejected
      // (strtoull would wrap "-1" to a huge number and silently stop at
      // "25junk"). The unit id must fit the uint32_t the intervention
      // carries.
      const ParsedNumber unit = parse_unsigned(
          unit_text, "--intervene unit id", 0xFFFFFFFFULL);
      const ParsedNumber tick = parse_unsigned(
          tick_text, "--intervene tick",
          std::numeric_limits<std::uint64_t>::max());
      if (!unit.ok || !tick.ok || unit.value == 0 || tick.value == 0) {
        outcome.ok = false;
        outcome.error = !unit.ok || !tick.ok
                            ? (unit.ok ? tick.error : unit.error)
                            : "--intervene needs a unit id and a tick >= 1";
        return outcome;
      }
      args.interventions.push_back("kill:" + std::to_string(unit.value) + ":" +
                                   std::to_string(tick.value));
      continue;
    }
    if (flag == "--contact-seek") {
      const char* value = require_value("--contact-seek");
      if (value == nullptr) {
        return outcome;
      }
      const std::string mode = value;
      if (mode != "on" && mode != "off") {
        outcome.ok = false;
        outcome.error =
            "--contact-seek expects 'on' or 'off', got '" + mode + "'";
        return outcome;
      }
      args.contact_seek_on = mode == "on";
      continue;
    }
    if (flag == "--nav") {
      const char* value = require_value("--nav");
      if (value == nullptr) {
        return outcome;
      }
      const std::string mode = value;
      if (mode == "v2" || mode == "on") {
        args.nav_v2 = true;
      } else if (mode == "v1" || mode == "off") {
        args.nav_v2 = false;
      } else {
        outcome.ok = false;
        outcome.error =
            "--nav expects 'v2' (default), 'v1', 'on' or 'off', got '" +
            mode + "'";
        return outcome;
      }
      continue;
    }
    if (flag == "--ai-avail" || flag == "--end" || flag == "--combat") {
      const char* value = require_value(flag.c_str());
      if (value == nullptr) {
        return outcome;
      }
      const std::string mode = value;
      const bool v2 = mode == "v2" || mode == "on";
      const bool v1 = mode == "v1" || mode == "off";
      if (!v2 && !v1) {
        outcome.ok = false;
        outcome.error =
            flag + " expects 'v2' (default), 'v1', 'on' or 'off', got '" +
            mode + "'";
        return outcome;
      }
      if (flag == "--ai-avail") {
        args.ai_avail_v2 = v2;
      } else if (flag == "--end") {
        args.end_v2 = v2;
      } else {
        args.combat_v2 = v2;
      }
      continue;
    }
    if (flag == "--orders") {
      const char* value = require_value("--orders");
      if (value == nullptr) {
        return outcome;
      }
      const std::string mode = value;
      if (mode != "on" && mode != "off") {
        outcome.ok = false;
        outcome.error = "--orders expects 'on' or 'off', got '" + mode + "'";
        return outcome;
      }
      args.orders_on = mode == "on";
      continue;
    }
    if (flag == "--threads") {
      const char* value = require_value("--threads");
      if (value == nullptr) {
        return outcome;
      }
      // m13 item 4: strict unsigned parse (the shared parser rejects
      // trailing characters, signs and overflow). 0 = hardware
      // concurrency (resolved by main); 1 (default) = inline phases.
      // Above 256 the pool would only add contention — refused with a
      // clear message instead.
      const ParsedNumber parsed = parse_unsigned(value, "--threads", 256);
      if (!parsed.ok) {
        outcome.ok = false;
        outcome.error = parsed.error;
        return outcome;
      }
      args.threads = static_cast<std::uint32_t>(parsed.value);
      args.threads_explicit = true;
      continue;
    }
    if (flag == "--profile-phases") {
      args.profile_phases = true;
      continue;
    }
    if (flag == "--check-invariants" ||
        flag.rfind("--check-invariants=", 0) == 0) {
      // M15 S1: optional-value flag — bare means every tick (N = 1),
      // "--check-invariants=N" every N ticks. Strict unsigned parse.
      std::uint32_t interval = 1;
      const std::string prefix = "--check-invariants=";
      if (flag.size() >= prefix.size()) {
        const std::string value = flag.substr(prefix.size());
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
        if (value.empty() || end == value.c_str() || *end != '\0' ||
            parsed == 0 || parsed > 100000) {
          outcome.ok = false;
          outcome.error =
              "--check-invariants expects a whole number in [1, 100000] "
              "after '=', got '" +
              value + "'";
          return outcome;
        }
        interval = static_cast<std::uint32_t>(parsed);
      }
      args.invariants_on = true;
      args.invariants_interval = interval;
      continue;
    }
    if (flag == "--general") {
      const char* value = require_value("--general");
      args.generals.emplace_back(value);
      continue;
    }
    if (flag == "--general-interval") {
      const char* value = require_value("--general-interval");
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(value, &end, 10);
      if (end == value || *end != '\0' || parsed == 0 || parsed > 10000) {
        outcome.error =
            "--general-interval must be a whole number in [1, 10000]";
        return outcome;
      }
      args.general_interval = static_cast<std::uint32_t>(parsed);
      continue;
    }
    if (flag == "--script") {
      const char* value = require_value("--script");
      if (value == nullptr) {
        return outcome;
      }
      if (std::string(value).empty()) {
        outcome.ok = false;
        outcome.error = "--script requires a non-empty file path";
        return outcome;
      }
      args.script_path = value;
      continue;
    }
    if (flag == "--swap-sides") {
      args.swap_sides = true;
      continue;
    }
    if (flag == "--tactic-scan") {
      const char* value = require_value("--tactic-scan");
      if (value == nullptr) {
        return outcome;
      }
      const std::string action = value;
      if (action != "search" && action != "help" && action != "flank") {
        outcome.ok = false;
        outcome.error =
            "--tactic-scan expects 'search', 'help' or 'flank', got '" +
            action + "'";
        return outcome;
      }
      args.tactic_scan = action;
      continue;
    }
    if (flag == "--tactics") {
      const char* value = require_value("--tactics");
      if (value == nullptr) {
        return outcome;
      }
      args.tactics_path = value;
      continue;
    }
    if (flag == "--formations") {
      const char* value = require_value("--formations");
      if (value == nullptr) {
        return outcome;
      }
      if (*value == '\0') {
        outcome.ok = false;
        outcome.error = "--formations requires a non-empty file path";
        return outcome;
      }
      args.formations_path = value;
      continue;
    }
    if (flag == "--commanders") {
      const char* value = require_value("--commanders");
      if (value == nullptr) {
        return outcome;
      }
      if (*value == '\0') {
        outcome.ok = false;
        outcome.error = "--commanders requires a non-empty file path";
        return outcome;
      }
      args.commanders_path = value;
      continue;
    }
    if (flag == "--squad-debug") {
      const char* value = require_value("--squad-debug");
      if (value == nullptr) {
        return outcome;
      }
      const ParsedNumber parsed =
          parse_unsigned(value, "--squad-debug", 0x7FFFFFFFULL);
      if (!parsed.ok) {
        outcome.ok = false;
        outcome.error = parsed.error;
        return outcome;
      }
      args.squad_debug_id = static_cast<std::int64_t>(parsed.value);
      continue;
    }
    if (flag == "--mod") {
      const char* value = require_value("--mod");
      if (value == nullptr) {
        return outcome;
      }
      if (*value == '\0') {
        outcome.ok = false;
        outcome.error = "--mod requires a non-empty directory path";
        return outcome;
      }
      args.mod_dir = value;
      continue;
    }
    if (flag == "--render-frame") {
      const char* value = require_value("--render-frame");
      if (value == nullptr) {
        return outcome;
      }
      const ParsedNumber parsed =
          parse_unsigned(value, "--render-frame", 0xFFFFFFFFFFFFFFFFULL);
      if (!parsed.ok) {
        outcome.ok = false;
        outcome.error = parsed.error;
        return outcome;
      }
      args.render_frame = parsed.value;
      continue;
    }
    if (flag == "--ui-script") {
      const char* value = require_value("--ui-script");
      if (value == nullptr) {
        return outcome;
      }
      if (std::string(value).empty()) {
        outcome.ok = false;
        outcome.error = "--ui-script requires a non-empty file path";
        return outcome;
      }
      args.ui_script = value;
      continue;
    }
    if (flag == "--render-frames") {
      const char* value = require_value("--render-frames");
      if (value == nullptr) {
        return outcome;
      }
      if (*value == '\0') {
        outcome.ok = false;
        outcome.error = "--render-frames requires a comma-separated tick list";
        return outcome;
      }
      args.render_frames = value;
      continue;
    }
    if (flag == "--out") {
      const char* value = require_value("--out");
      if (value == nullptr) {
        return outcome;
      }
      if (*value == '\0') {
        outcome.ok = false;
        outcome.error = "--out requires a non-empty file path";
        return outcome;
      }
      args.render_out = value;
      continue;
    }
    if (flag == "--out-prefix") {
      const char* value = require_value("--out-prefix");
      if (value == nullptr) {
        return outcome;
      }
      if (*value == '\0') {
        outcome.ok = false;
        outcome.error = "--out-prefix requires a non-empty path prefix";
        return outcome;
      }
      args.render_out_prefix = value;
      continue;
    }
    if (flag == "--ascii") {
      const char* value = require_value("--ascii");
      if (value == nullptr) {
        return outcome;
      }
      const ParsedNumber parsed =
          parse_unsigned(value, "--ascii", 0xFFFFFFFFFFFFFFFFULL);
      if (!parsed.ok) {
        outcome.ok = false;
        outcome.error = parsed.error;
        return outcome;
      }
      args.ascii_tick = parsed.value;
      continue;
    }
    if (flag == "--export-json") {
      const char* value = require_value("--export-json");
      if (value == nullptr) {
        return outcome;
      }
      args.export_json = true;
      args.export_path = value;
      continue;
    }

    outcome.ok = false;
    outcome.error = "unknown argument '" + flag + "'";
    return outcome;
  }

  if (args.units < 2) {
    outcome.ok = false;
    outcome.error = "--units must be at least 2 (one unit per faction)";
    return outcome;
  }
  if (args.units_explicit && !args.scenario_path.empty()) {
    outcome.ok = false;
    outcome.error =
        "--units cannot be combined with --scenario: the scenario defines "
        "the army composition";
    return outcome;
  }
  if (args.ticks < 1) {
    outcome.ok = false;
    outcome.error = "--ticks must be at least 1";
    return outcome;
  }
  if (args.grid_cell_explicit && !args.grid_mode) {
    outcome.ok = false;
    outcome.error =
        "--grid-cell cannot be combined with --neighbor-mode brute";
    return outcome;
  }
  if (args.ai_debug_tick_explicit && args.ai_debug_unit == 0) {
    outcome.ok = false;
    outcome.error = "--ai-debug-tick requires --ai-debug <unit id>";
    return outcome;
  }
  if (args.ai_debug_unit != 0 && !args.utility_ai) {
    outcome.ok = false;
    outcome.error = "--ai-debug requires --ai-mode utility";
    return outcome;
  }
  if (args.morale_explicit && !args.utility_ai) {
    outcome.ok = false;
    outcome.error =
        "--morale cannot be combined with --ai-mode legacy (legacy ignores "
        "morale to reproduce M1-M3)";
    return outcome;
  }
  if (args.squads_explicit && !args.utility_ai) {
    outcome.ok = false;
    outcome.error =
        "--squads cannot be combined with --ai-mode legacy (legacy ignores "
        "squads to reproduce M1-M3)";
    return outcome;
  }
  if (args.squad_debug_id >= 0 && !args.utility_ai) {
    outcome.ok = false;
    outcome.error = "--squad-debug requires --ai-mode utility";
    return outcome;
  }
  if (!args.mod_dir.empty()) {
    std::error_code ec;
    if (!std::filesystem::is_directory(args.mod_dir, ec)) {
      outcome.ok = false;
      outcome.error =
          "--mod directory '" + args.mod_dir + "' does not exist";
      return outcome;
    }
  }
  if (args.render_frame > 0 && args.render_out.empty()) {
    outcome.ok = false;
    outcome.error =
        "--render-frame requires --out <file.png> for the output path";
    return outcome;
  }
  if (!args.render_frames.empty() && args.render_out_prefix.empty()) {
    outcome.ok = false;
    outcome.error =
        "--render-frames requires --out-prefix <dir/name> for the outputs";
    return outcome;
  }
  if (args.render_frame > 0 && !args.render_frames.empty()) {
    outcome.ok = false;
    outcome.error =
        "--render-frame and --render-frames are mutually exclusive";
    return outcome;
  }
  if (args.export_json && args.export_path.empty()) {
    outcome.ok = false;
    outcome.error = "--export-json requires a non-empty file path";
    return outcome;
  }
  return outcome;
}

void apply_side_swap(sim::SimulationConfig& config) {
  // --swap-sides (M10): mirror the whole setup — compositions, explicit
  // objectives, squad plans and spawn patterns swap with the bands
  // (config.swap_sides flips which band each faction holds). Unit
  // factions themselves are untouched: Red simply deploys in the east.
  config.composition[static_cast<std::size_t>(sim::Faction::Red)]
      .swap(config.composition[static_cast<std::size_t>(sim::Faction::Blue)]);
  config.squad_plans[static_cast<std::size_t>(sim::Faction::Red)]
      .swap(config.squad_plans[static_cast<std::size_t>(sim::Faction::Blue)]);
  // m11a: the explicit-objective swap must happen exactly once. The
  // previous per-faction loop swapped them on the Red iteration and
  // swapped them right back on the Blue one, so explicit objectives
  // never moved and a swapped battle parked each army on the objective
  // inside its own spawn band (the universal 0/0/N tick-limit draws on
  // the earth maps; every earth scenario sets explicit objectives).
  // The once-only swaps. (The M10 loop form swapped per faction, which
  // undid itself on the second iteration — objectives AND spawn
  // patterns never actually moved.)
  if (config.objectives[static_cast<std::size_t>(sim::Faction::Red)]
          .explicit_set ||
      config.objectives[static_cast<std::size_t>(sim::Faction::Blue)]
          .explicit_set) {
    std::swap(
        config.objectives[static_cast<std::size_t>(sim::Faction::Red)],
        config.objectives[static_cast<std::size_t>(sim::Faction::Blue)]);
  }
  std::swap(
      config.spawn_patterns[static_cast<std::size_t>(sim::Faction::Red)],
      config.spawn_patterns[static_cast<std::size_t>(sim::Faction::Blue)]);
}

void print_usage(std::ostream& out) {
  out << "autobattle — deterministic 2D autonomous battle simulation (M6)\n"
      << "\n"
      << "Usage: battle_cli [options]\n"
      << "\n"
      << "Options:\n"
      << "  --help                  Show this help and exit\n"
      << "  --seed <n>              Random seed (64-bit unsigned, default 42)\n"
      << "  --units <n>             M1-style total unit count: an infantry-only\n"
      << "                         split between the two factions (Red gets the\n"
      << "                         extra odd unit; default 500, minimum 2,\n"
      << "                         maximum 20000). Cannot be combined with\n"
      << "                         --scenario.\n"
      << "  --ticks <n>             Tick limit (default 3000, minimum 1). Use\n"
      << "                         600 with the perf scenarios to benchmark:\n"
      << "                         the report shows ticks actually run, ticks/s\n"
      << "                         and ms per tick.\n"
      << "  --config <file>         Unit type definitions in JSON\n"
      << "                         (default: data/units.json)\n"
      << "  --scenario <file>       Battle scenario in JSON: battlefield size,\n"
      << "                         spawn band, per-faction army composition and\n"
      << "                         optional objectives (default:\n"
      << "                         data/scenarios/default.json; see also\n"
      << "                         perf-5000.json / perf-20000.json)\n"
      << "  --neighbor-mode <mode>  Nearest-enemy search for legacy targeting:\n"
      << "                         spatial 'grid' (default, M3) or 'brute' (the\n"
      << "                         M1/M2 scan; identical results). Utility-mode\n"
      << "                         perception always uses the grid.\n"
      << "  --grid-cell <size>      Spatial grid cell size in world units\n"
      << "                         (default 16.0; grid mode only)\n"
      << "  --ai-mode <mode>        'utility' (default, M4: sight-limited\n"
      << "                         perception, personality-weighted action\n"
      << "                         scoring) or 'legacy' (the exact M1-M3 AI;\n"
      << "                         reproduces every reference result)\n"
      << "  --personalities <file>  Personality archetypes in JSON\n"
      << "                         (default: data/personalities.json)\n"
      << "  --morale <mode>         'on' (default, M5: morale, routing and\n"
      << "                         recovery; see docs/morale.md) or 'off'\n"
      << "                         (bit-identical to the M4 AI)\n"
      << "  --morale-config <file>  Morale configuration in JSON\n"
      << "                         (default: data/morale.json)\n"
      << "  --squads <mode>         'on' (default, M6: squads, formations,\n"
      << "                         commander orders and succession; see\n"
      << "                         docs/squads.md) or 'off' (bit-identical to\n"
      << "                         the M5 AI). Squads need a scenario with a\n"
      << "                         'squads' section; without one, on is a\n"
      << "                         no-op. Ignored by legacy mode.\n"
      << "  --formations <file>     Formation and steering configuration in\n"
      << "                         JSON (default: data/formations.json)\n"
      << "  --commanders <file>     Commander order rules in JSON\n"
      << "                         (default: data/commanders.json)\n"
      << "  --squad-debug <id>      Print one squad's members, commander,\n"
      << "                         order and cohesion at the end of the run\n"
      << "                         (utility mode)\n"
      << "  --render-frame <tick>   Render the battle at <tick> to a PNG\n"
      << "                         (1280x720, camera fit to the world); needs\n"
      << "                         --out <file.png> (M7 viewer pipeline)\n"
      << "  --render-frames a,b,c   Render several ticks, one PNG each; needs\n"
      << "                         --out-prefix <dir/name> (files are\n"
      << "                         <prefix>-<tick:6>.png)\n"
      << "  --ui-script <file>      Headless UI harness (m11c): drive the\n"
      << "                         demo control panel with scripted key /\n"
      << "                         mouse events ({width, height, tick, out,\n"
      << "                         events: [{frame, type, ...}]}); one PNG\n"
      << "                         per frame plus per-frame state lines\n"
      << "  --ascii <tick>          Print a compact terminal map at <tick>\n"
      << "  --mod <dir>             Load a mod directory (M8): replaces\n"
      << "                         same-named data files wholesale; may add\n"
      << "                         scenarios/ and maps/; invalid mods are\n"
      << "                         rejected with nothing partially loaded\n"
      << "  --ai-debug <unit id>    Print one utility unit's health, personality,\n"
      << "                         visible counts, all action scores and the\n"
      << "                         chosen action for --ai-debug-tick, or the\n"
      << "                         first tick it sees an enemy (utility mode)\n"
      << "  --ai-debug-tick <n>     Explicit 1-based tick for --ai-debug\n"
      << "  --export-json <file>    Write the final battle state as JSON\n"
      << "  --trace <file>          Write a navigation trace line every\n"
      << "                         --trace-interval ticks (per-faction\n"
      << "                         centroid, mean move, still fraction,\n"
      << "                         Advance fraction, x histogram). Pure\n"
      << "                         observation: the battle is unchanged\n"
      << "  --trace-interval <n>    Trace cadence in ticks [1, 100000]\n"
      << "                         (default 50; needs --trace)\n"
      << "  --nav <v1|v2>           Navigation generation (m11a): v2\n"
      << "                         (default) marches formation members\n"
      << "                         along the flow field when far from\n"
      << "                         their slot and fixes the --swap-sides\n"
      << "                         explicit-objective swap; v1 is the\n"
      << "                         exact M10 navigation (byte-identical)\n"
      << "  --ai-avail <v1|v2>      Action availability generation (m13):\n"
      << "                         v2 (default) never lifts an unavailable\n"
      << "                         action (the M10 regroup pseudo-\n"
      << "                         availability of Advance), caps Help per\n"
      << "                         ally and decays stale contact estimates;\n"
      << "                         v1 is the exact milestone-12 scoring\n"
      << "  --end <v1|v2>           Termination generation (m13): v2\n"
      << "                         (default) also ends a battle when a\n"
      << "                         faction's active units stay below\n"
      << "                         max(5, 5% of initial) for 60 ticks\n"
      << "                         (data/morale.json); v1 keeps the\n"
      << "                         milestone-12 end rules\n"
      << "  --combat <v1|v2>        Combat depth generation (m13): v2\n"
      << "                         (default) adds facing (flank x1.25 /\n"
      << "                         rear x1.5 damage + rear morale loss),\n"
      << "                         cavalry charges, fatigue, experience,\n"
      << "                         commander auras and the defensive\n"
      << "                         stance of high-cohesion Hold squads\n"
      << "                         (data/combat.json); v1 is the exact\n"
      << "                         milestone-12 combat. Never runs in\n"
      << "                         legacy mode\n"
      << "  --orders <on|off>       Player orders (m11b): on (default)\n"
      << "                         enables issue_order; off makes every\n"
      << "                         order inert (and rejects --script)\n"
      << "  --script <file>         Timed player orders from a JSON\n"
      << "                         array (m11b): {tick, faction,\n"
      << "                         squads|[\"all\"], order: moveto|\n"
      << "                         attackmove|hold|retreat|regroup|\n"
      << "                         formation, x, y, formation}; recorded\n"
      << "                         into --record replays, re-applied by\n"
      << "                         --replay\n"
      << "  --threads <n>           Deterministic multithreading (m13):\n"
      << "                         1 (default) runs every phase inline;\n"
      << "                         n >= 2 runs perception and decision\n"
      << "                         on a persistent worker pool in fixed\n"
      << "                         chunks; 0 = one worker per hardware\n"
      << "                         core. Output is byte-identical for\n"
      << "                         every thread count\n"
      << "  --profile-phases        Print per-phase timing after the run\n"
      << "  --check-invariants[=n]  Validate state every n ticks (default 1);\n"
      << "                          on failure print the violation and write\n"
      << "                          a replay, exit code 3 (default off)\n"
      << "                         (perception / decision / resolution /\n"
      << "                         commit / total; clock reads only, the\n"
      << "                         battle is unchanged)\n"
      << "\n"
      << "Without --units or --scenario, the default scenario runs (with the\n"
      << "shipped data files: 250 infantry per faction). The battle ends when\n"
      << "one faction has no surviving units or when the tick limit is reached.\n"
      << "The CLI prints winner, survivors, deaths, per-type statistics,\n"
      << "termination reason, timing, throughput, peak memory and the JSON\n"
      << "export location. Editing the JSON files changes battles without\n"
      << "recompiling.\n";
}

}  // namespace cli
