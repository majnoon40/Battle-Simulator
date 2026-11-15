// M9 — every generated Earth map and scenario must load and run through
// battle_cli: exact palette match, objectives passable and reachable,
// spawn bands deployable (the M8 validation). Runs one tick and checks
// the format-6 export.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

std::string read_text(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

int run_cli(const std::string& arguments) {
  const std::string command =
      "cd \"" TEST_DATA_DIR "/..\" && \"" BATTLE_CLI_PATH "\" " + arguments +
      " > /tmp/m9_cli_out.txt 2>&1";
  return std::system(command.c_str());
}

}  // namespace

TEST(EarthMaps, AllGeneratedRegionsLoadAndRun) {
  const char* regions[] = {"earth-lebanon", "earth-alps-innsbruck",
                           "earth-nile-delta"};
  for (const char* region : regions) {
    const int code = run_cli(std::string("--scenario ") + region +
                             " --seed 7 --ticks 1 --variance off "
                             "--export-json /tmp/m9_earth_export.json");
    ASSERT_EQ(code, 0) << region << " failed to load (see "
                          "/tmp/m9_cli_out.txt)";
    const std::string text = read_text("/tmp/m9_earth_export.json");
    EXPECT_NE(text.find("\"format_version\": 6"), std::string::npos)
        << region;
    EXPECT_NE(text.find("\"map\""), std::string::npos) << region;
  }
}
