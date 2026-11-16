#include "cli/replay.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

namespace cli {

// Deterministic round-trip formatting for order points (classic locale,
// shortest of the 17-significant-digit round-trip).
[[nodiscard]] std::string format_point(double value) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << value;
  return out.str();
}

namespace {

[[nodiscard]] bool parse_double(const std::string& text, double& out) {
  if (text.empty()) {
    return false;
  }
  std::size_t used = 0;
  try {
    out = std::stod(text, &used);
  } catch (...) {
    return false;
  }
  return used == text.size();
}

}  // namespace

bool parse_order_script(const std::string& path,
                        std::vector<OrderIntervention>& out,
                        std::string& error) {
  std::ifstream file(path);
  if (!file) {
    error = "cannot open order script '" + path + "' for reading";
    return false;
  }
  nlohmann::json root;
  try {
    file >> root;
  } catch (const nlohmann::json::exception& parse_error) {
    // json::exception, not just parse_error: a number overflow (1e999)
    // throws out_of_range.406, which used to escape and abort the CLI.
    error = std::string("order script is not valid JSON: ") +
            parse_error.what();
    return false;
  }
  if (!root.is_array()) {
    error = "order script must be a JSON array of order objects";
    return false;
  }
  for (const nlohmann::json& entry : root) {
    if (!entry.is_object() || !entry.contains("tick") ||
        !entry.contains("faction") || !entry.contains("squads") ||
        !entry.contains("order")) {
      error =
          "every order script entry needs tick, faction, squads and order";
      return false;
    }
    OrderIntervention order;
    if (!entry["tick"].is_number_unsigned() ||
        entry["tick"].get<std::uint64_t>() == 0) {
      error = "order script: tick must be an unsigned integer >= 1";
      return false;
    }
    order.tick = entry["tick"].get<std::uint64_t>();
    if (!entry["faction"].is_string()) {
      error = "order script: faction must be a string ('red' or 'blue')";
      return false;
    }
    const std::string faction = entry["faction"].get<std::string>();
    if (faction != "red" && faction != "blue") {
      error = "order script: faction must be 'red' or 'blue', got '" +
              faction + "'";
      return false;
    }
    order.red = faction == "red";
    if (entry["squads"].is_string()) {
      if (entry["squads"].get<std::string>() != "all") {
        error = "order script: squads must be \"all\" or an id array";
        return false;
      }
      order.squads_all = true;
    } else if (entry["squads"].is_array()) {
      if (entry["squads"].empty()) {
        error = "order script: squads must not be an empty array";
        return false;
      }
      for (const nlohmann::json& squad : entry["squads"]) {
        if (!squad.is_number_unsigned()) {
          error = "order script: squad ids must be unsigned integers";
          return false;
        }
        order.squads.push_back(squad.get<std::uint32_t>());
      }
    } else {
      error = "order script: squads must be \"all\" or an id array";
      return false;
    }
    if (!entry["order"].is_string()) {
      error = "order script: order must be a string (moveto, attackmove, "
              "hold, retreat, regroup or formation)";
      return false;
    }
    const std::string type = entry["order"].get<std::string>();
    if (!sim::parse_player_order_type(type, order.type)) {
      error = "order script: unknown order type '" + type +
              "' (expected moveto, attackmove, hold, retreat, regroup "
              "or formation)";
      return false;
    }
    const bool needs_point = order.type == sim::PlayerOrderType::MoveTo ||
                             order.type ==
                                 sim::PlayerOrderType::AttackMove;
    const bool optional_point =
        order.type == sim::PlayerOrderType::Retreat;
    if (needs_point || optional_point) {
      if (entry.contains("x") && entry.contains("y")) {
        if (!entry["x"].is_number() || !entry["y"].is_number()) {
          error = "order script: x and y must be numbers";
          return false;
        }
        order.x = entry["x"].get<double>();
        order.y = entry["y"].get<double>();
        if (!std::isfinite(order.x) || !std::isfinite(order.y)) {
          error = "order script: x and y must be finite numbers";
          return false;
        }
        order.has_point = true;
      } else if (needs_point) {
        error = "order script: " + type + " requires x and y";
        return false;
      }
    }
    if (order.type == sim::PlayerOrderType::ChangeFormation) {
      if (!entry.contains("formation") ||
          !entry["formation"].is_string()) {
        error = "order script: formation requires a formation id string";
        return false;
      }
      order.formation = entry["formation"].get<std::string>();
    }
    if (order.has_point && order.type == sim::PlayerOrderType::Retreat &&
        order.x == 0.0 && order.y == 0.0) {
      // (0, 0) is the "no point" sentinel in the issue API.
      order.has_point = false;
    }
    out.push_back(std::move(order));
  }
  // Ascending tick, stable file order within a tick.
  std::stable_sort(out.begin(), out.end(),
                   [](const OrderIntervention& a, const OrderIntervention& b) {
                     return a.tick < b.tick;
                   });
  return true;
}

namespace {

[[nodiscard]] bool read_lines(const std::string& path,
                              std::vector<std::string>& lines,
                              std::string& error) {
  std::ifstream file(path);
  if (!file) {
    error = "cannot open replay file '" + path + "' for reading";
    return false;
  }
  std::string line;
  while (std::getline(file, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
  return true;
}

[[nodiscard]] bool parse_u64(const std::string& text, std::uint64_t& out) {
  if (text.empty()) {
    return false;
  }
  std::size_t used = 0;
  try {
    out = std::stoull(text, &used);
  } catch (...) {
    return false;
  }
  return used == text.size();
}

}  // namespace

bool parse_replay(const std::string& path, ReplayData& out,
                  std::string& error) {
  std::vector<std::string> lines;
  if (!read_lines(path, lines, error)) {
    return false;
  }
  if (lines.empty() || lines[0] != "BATTLESIM REPLAY 1") {
    error = "not a battle replay file (missing 'BATTLESIM REPLAY 1' magic)";
    return false;
  }
  bool have_seed = false;
  bool have_ticks = false;
  bool have_scenario = false;
  std::uint64_t last_hash_tick = 0;
  std::uint64_t last_intervention_tick = 0;
  std::uint64_t last_order_tick = 0;
  for (std::size_t i = 1; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    const std::size_t space = line.find(' ');
    const std::string key = space == std::string::npos
                                ? line
                                : line.substr(0, space);
    const std::string rest =
        space == std::string::npos ? "" : line.substr(space + 1);
    if (key == "version") {
      out.version = rest;
    } else if (key == "scenario") {
      out.scenario = rest;
      have_scenario = true;
    } else if (key == "mod") {
      out.mod = rest;
    } else if (key == "seed") {
      if (!parse_u64(rest, out.seed)) {
        error = "bad seed line: '" + line + "'";
        return false;
      }
      have_seed = true;
    } else if (key == "ticks") {
      if (!parse_u64(rest, out.ticks) || out.ticks == 0) {
        error = "bad ticks line: '" + line + "'";
        return false;
      }
      have_ticks = true;
    } else if (key == "flags") {
      std::istringstream words(rest);
      std::string pair;
      while (words >> pair) {
        const std::size_t eq = pair.find('=');
        if (eq == std::string::npos) {
          error = "bad flags entry '" + pair + "'";
          return false;
        }
        const std::string name = pair.substr(0, eq);
        const std::string value = pair.substr(eq + 1);
        if (name == "variance") {
          out.variance = value;
        } else if (name == "contact_seek") {
          out.contact_seek = value;
        } else if (name == "neighbor") {
          out.neighbor = value;
        } else if (name == "squads") {
          out.squads = value;
        } else if (name == "swap_sides") {
          out.swap_sides = value;
        } else if (name == "nav") {
          if (value != "v1" && value != "v2") {
            error = "bad nav flag value '" + value + "' in replay";
            return false;
          }
          out.nav = value;
        } else if (name == "orders") {
          if (value != "on" && value != "off") {
            error = "bad orders flag value '" + value + "' in replay";
            return false;
          }
          out.orders = value;
        } else {
          error = "unknown flag '" + name + "' in replay";
          return false;
        }
      }
    } else if (key == "intervene") {
      std::istringstream words(rest);
      std::string kind;
      std::string unit_text;
      std::string tick_text;
      if (!(words >> kind >> unit_text >> tick_text) || kind != "kill") {
        error = "bad intervene line: '" + line + "'";
        return false;
      }
      Intervention intervention;
      std::uint64_t unit = 0;
      std::uint64_t tick = 0;
      if (!parse_u64(unit_text, unit) || unit == 0 || unit > 0xffffffffULL ||
          !parse_u64(tick_text, tick) || tick == 0 ||
          tick < last_intervention_tick) {
        error = "bad intervene line: '" + line + "'";
        return false;
      }
      intervention.unit = static_cast<std::uint32_t>(unit);
      intervention.tick = tick;
      last_intervention_tick = intervention.tick;
      out.interventions.push_back(intervention);
    } else if (key == "order") {
      // m11b: order <tick> <red|blue> <squads|all> <type> [args]
      //   moveto/attackmove: x y   |   retreat: [x y]
      //   formation: <formation id> |   hold/regroup: (none)
      std::istringstream words(rest);
      std::string tick_text;
      std::string faction;
      std::string squads_text;
      std::string type_text;
      if (!(words >> tick_text >> faction >> squads_text >> type_text)) {
        error = "bad order line: '" + line + "'";
        return false;
      }
      OrderIntervention order;
      if (!parse_u64(tick_text, order.tick) || order.tick == 0 ||
          order.tick < last_order_tick) {
        error = "bad order line (tick must ascend): '" + line + "'";
        return false;
      }
      if (faction != "red" && faction != "blue") {
        error = "bad order line (faction must be red or blue): '" + line +
                "'";
        return false;
      }
      order.red = faction == "red";
      if (squads_text == "all") {
        order.squads_all = true;
      } else {
        std::string squad_id;
        std::stringstream squad_stream(squads_text);
        while (std::getline(squad_stream, squad_id, ',')) {
          std::uint64_t id = 0;
          if (!parse_u64(squad_id, id) || id > 0xffffffffULL) {
            error = "bad order line (squad ids): '" + line + "'";
            return false;
          }
          order.squads.push_back(static_cast<std::uint32_t>(id));
        }
        if (order.squads.empty()) {
          error = "bad order line (no squad ids): '" + line + "'";
          return false;
        }
      }
      if (!sim::parse_player_order_type(type_text, order.type)) {
        error = "bad order line (unknown type '" + type_text + "'): '" +
                line + "'";
        return false;
      }
      if (order.type == sim::PlayerOrderType::ChangeFormation) {
        if (!(words >> order.formation)) {
          error = "bad order line (formation needs an id): '" + line + "'";
          return false;
        }
      } else if (order.type == sim::PlayerOrderType::MoveTo ||
                 order.type == sim::PlayerOrderType::AttackMove) {
        std::string x_text;
        std::string y_text;
        if (!(words >> x_text >> y_text) || !parse_double(x_text, order.x) ||
            !parse_double(y_text, order.y)) {
          error = "bad order line (" + type_text + " needs x y): '" +
                  line + "'";
          return false;
        }
        order.has_point = true;
      } else if (order.type == sim::PlayerOrderType::Retreat) {
        std::string x_text;
        std::string y_text;
        if ((words >> x_text >> y_text) && parse_double(x_text, order.x) &&
            parse_double(y_text, order.y)) {
          if (order.x != 0.0 || order.y != 0.0) {
            order.has_point = true;
          }
        }
      }
      last_order_tick = order.tick;
      out.order_interventions.push_back(std::move(order));
    } else if (key == "hash") {
      std::istringstream words(rest);
      std::string tick_text;
      std::string hash;
      if (!(words >> tick_text >> hash) || hash.size() != 32) {
        error = "bad hash line: '" + line + "'";
        return false;
      }
      std::uint64_t tick = 0;
      if (!parse_u64(tick_text, tick) || tick <= last_hash_tick) {
        error = "bad or non-ascending hash tick: '" + line + "'";
        return false;
      }
      last_hash_tick = tick;
      out.hashes.emplace_back(tick, hash);
    } else {
      error = "unknown replay line: '" + line + "'";
      return false;
    }
  }
  if (out.version.empty()) {
    error = "replay file has no version line";
    return false;
  }
  if (!have_scenario || out.scenario.empty()) {
    error = "replay file has no scenario line";
    return false;
  }
  if (!have_seed) {
    error = "replay file has no seed line";
    return false;
  }
  if (!have_ticks) {
    error = "replay file has no ticks line";
    return false;
  }
  return true;
}

bool write_replay(const std::string& path, const ReplayData& data) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }
  file << "BATTLESIM REPLAY 1\n";
  file << "version " << data.version << "\n";
  file << "scenario " << data.scenario << "\n";
  file << "mod " << data.mod << "\n";
  file << "seed " << data.seed << "\n";
  file << "ticks " << data.ticks << "\n";
  file << "flags variance=" << data.variance
       << " contact_seek=" << data.contact_seek
       << " neighbor=" << data.neighbor << " squads=" << data.squads
       << " swap_sides=" << data.swap_sides << " nav=" << data.nav
       << " orders=" << data.orders << "\n";
  for (const Intervention& intervention : data.interventions) {
    file << "intervene kill " << intervention.unit << " "
         << intervention.tick << "\n";
  }
  for (const OrderIntervention& order : data.order_interventions) {
    file << "order " << order.tick << " " << (order.red ? "red" : "blue")
         << " ";
    if (order.squads_all) {
      file << "all";
    } else {
      for (std::size_t i = 0; i < order.squads.size(); ++i) {
        file << (i > 0 ? "," : "") << order.squads[i];
      }
    }
    file << " " << sim::to_string(order.type);
    if (order.type == sim::PlayerOrderType::ChangeFormation) {
      file << " " << order.formation;
    } else if (order.has_point) {
      file << " " << format_point(order.x) << " " << format_point(order.y);
    }
    file << "\n";
  }
  for (const auto& [tick, hash] : data.hashes) {
    file << "hash " << tick << " " << hash << "\n";
  }
  return static_cast<bool>(file);
}

}  // namespace cli
