#include "cli/replay.hpp"

#include <fstream>
#include <sstream>

namespace cli {
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
       << " swap_sides=" << data.swap_sides << "\n";
  for (const Intervention& intervention : data.interventions) {
    file << "intervene kill " << intervention.unit << " "
         << intervention.tick << "\n";
  }
  for (const auto& [tick, hash] : data.hashes) {
    file << "hash " << tick << " " << hash << "\n";
  }
  return static_cast<bool>(file);
}

}  // namespace cli
