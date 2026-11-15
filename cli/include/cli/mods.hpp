#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace cli {

// Mod overlays (M8): --mod <dir> replaces whole data files of the same
// name. A mod directory may contain units.json, terrain.json,
// personalities.json, morale.json, formations.json, commanders.json, a
// scenarios/ subdirectory and a maps/ subdirectory. There is no partial
// merging: a mod's units.json fully replaces data/units.json, and an
// invalid mod is rejected wholesale (see validate_mod — every file it
// contains must parse before anything is loaded).
namespace mods {

// The data files a mod may replace, in the order they are validated.
[[nodiscard]] std::vector<std::string> data_file_names();

// True when <mod_dir>/<name> exists.
[[nodiscard]] bool has_file(const std::string& mod_dir,
                            const std::string& name);

// The path a data file should be loaded from: the mod's copy when it has
// one, otherwise the built-in data/ path.
[[nodiscard]] std::filesystem::path data_file(const std::string& mod_dir,
                                              const std::string& name);

// Resolves a --scenario argument that is not an existing path: the mod's
// scenarios/<name>.json first, then data/scenarios/<name>.json. Empty
// when neither exists.
[[nodiscard]] std::filesystem::path resolve_scenario(
    const std::string& mod_dir, const std::string& scenario_arg);

// Resolves a scenario's map file reference: the mod's maps/<file> first,
// then data/maps/<file>. Empty when neither exists.
[[nodiscard]] std::filesystem::path resolve_map(const std::string& mod_dir,
                                                const std::string& map_file);

// Whole-mod validation: parses every data file the mod provides (units,
// terrain, personalities, morale, formations, commanders) and reports the
// FIRST failure. Returns an empty vector on success (all mod files
// valid), or every problem found, one readable message per line. Nothing
// is partially loaded when any message is returned.
[[nodiscard]] std::vector<std::string> validate_mod(const std::string& mod_dir);

}  // namespace mods
}  // namespace cli
