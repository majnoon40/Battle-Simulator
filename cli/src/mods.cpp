#include "cli/mods.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "sim/morale.hpp"
#include "sim/personalities.hpp"
#include "sim/squads.hpp"
#include "sim/terrain.hpp"
#include "sim/unit_types.hpp"

namespace cli {
namespace mods {
namespace {

std::string read_text(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

}  // namespace

std::vector<std::string> data_file_names() {
  return {"units.json",     "terrain.json",   "personalities.json",
          "morale.json",    "formations.json", "commanders.json"};
}

bool has_file(const std::string& mod_dir, const std::string& name) {
  if (mod_dir.empty()) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::exists(
      std::filesystem::path(mod_dir) / name, ec);
}

std::filesystem::path data_file(const std::string& mod_dir,
                                const std::string& name) {
  if (has_file(mod_dir, name)) {
    return std::filesystem::path(mod_dir) / name;
  }
  return std::filesystem::path("data") / name;
}

std::filesystem::path resolve_scenario(const std::string& mod_dir,
                                       const std::string& scenario_arg) {
  std::error_code ec;
  if (!mod_dir.empty()) {
    const std::filesystem::path in_mod =
        std::filesystem::path(mod_dir) / "scenarios" /
        (scenario_arg + ".json");
    if (std::filesystem::exists(in_mod, ec)) {
      return in_mod;
    }
  }
  const std::filesystem::path builtin =
      std::filesystem::path("data") / "scenarios" / (scenario_arg + ".json");
  if (std::filesystem::exists(builtin, ec)) {
    return builtin;
  }
  return {};
}

std::filesystem::path resolve_map(const std::string& mod_dir,
                                  const std::string& map_file) {
  std::error_code ec;
  if (!mod_dir.empty()) {
    const std::filesystem::path in_mod =
        std::filesystem::path(mod_dir) / "maps" / map_file;
    if (std::filesystem::exists(in_mod, ec)) {
      return in_mod;
    }
  }
  const std::filesystem::path builtin =
      std::filesystem::path("data") / "maps" / map_file;
  if (std::filesystem::exists(builtin, ec)) {
    return builtin;
  }
  return {};
}

std::vector<std::string> validate_mod(const std::string& mod_dir) {
  std::vector<std::string> problems;
  if (mod_dir.empty()) {
    return problems;
  }
  const auto check = [&](const std::string& name, auto parse) {
    if (!has_file(mod_dir, name)) {
      return;
    }
    const std::filesystem::path path =
        std::filesystem::path(mod_dir) / name;
    try {
      parse(read_text(path));
    } catch (const std::exception& error) {
      problems.push_back("mod file '" + path.string() +
                         "' is invalid: " + error.what());
    }
  };
  check("units.json", [](const std::string& text) {
    sim::parse_unit_types(text);
  });
  check("terrain.json", [](const std::string& text) {
    sim::parse_terrain_types(text);
  });
  check("personalities.json", [](const std::string& text) {
    sim::parse_personalities(text);
  });
  check("morale.json",
        [](const std::string& text) { sim::parse_morale(text); });
  check("formations.json", [](const std::string& text) {
    sim::parse_formations(text);
  });
  check("commanders.json", [](const std::string& text) {
    sim::parse_commanders(text);
  });
  return problems;
}

}  // namespace mods
}  // namespace cli
