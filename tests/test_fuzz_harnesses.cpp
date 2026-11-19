// m13 item 6 — the fuzzing harnesses over the seed corpus. The harness
// functions are plain code (fuzz/harnesses.cpp); the clang/libFuzzer
// builds (BUILD_FUZZING=ON) explore beyond the corpus — this test pins
// the contract (never crash, never throw) on every committed seed and
// runs inside the normal ctest suite, so the ASan/UBSan CI job covers
// the harness code paths too.
#include "fuzz/harnesses.hpp"

#include <cstdint>
#include <dirent.h>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct Harness {
  const char* name;
  int (*fn)(const std::uint8_t*, std::size_t);
};

const Harness kHarnesses[] = {
    {"unit_types", fuzz::unit_types_harness},
    {"scenario", fuzz::scenario_harness},
    {"personalities", fuzz::personalities_harness},
    {"morale", fuzz::morale_harness},
    {"tactics", fuzz::tactics_harness},
    {"formations", fuzz::formations_harness},
    {"commanders", fuzz::commanders_harness},
    {"combat", fuzz::combat_harness},
    {"terrain_grid", fuzz::terrain_grid_harness},
    {"settings", fuzz::settings_harness},
    {"replay", fuzz::replay_harness},
    {"order_script", fuzz::order_script_harness},
    {"simulation", fuzz::simulation_harness},
};

std::vector<std::string> corpus_files(const std::string& harness) {
  std::vector<std::string> files;
  const std::string dir = std::string(FUZZ_CORPUS_DIR) + "/" + harness;
  if (DIR* handle = opendir(dir.c_str()); handle != nullptr) {
    while (const dirent* entry = readdir(handle)) {
      const std::string name = entry->d_name;
      if (name != "." && name != "..") {
        files.push_back(dir + "/" + name);
      }
    }
    closedir(handle);
  }
  EXPECT_FALSE(files.empty()) << "no corpus seeds for " << harness;
  return files;
}

}  // namespace

TEST(FuzzHarnessTest, EverySeedCorpusFilePassesEveryHarness) {
  for (const Harness& harness : kHarnesses) {
    for (const std::string& path : corpus_files(harness.name)) {
      std::ifstream file(path, std::ios::binary);
      ASSERT_TRUE(file.good()) << path;
      const std::string text((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
      // The contract: return 0, never crash, never escape an exception.
      EXPECT_EQ(harness.fn(reinterpret_cast<const std::uint8_t*>(text.data()),
                           text.size()),
                0)
          << harness.name << " on " << path;
    }
  }
}

TEST(FuzzHarnessTest, CrossSeedsPassToo) {
  // Every harness must also survive other harnesses' seeds (parsers
  // face mixed directories in the wild through --mod, for example).
  for (const Harness& harness : kHarnesses) {
    for (const char* other : {"scenario", "unit_types", "replay",
                              "settings"}) {
      for (const std::string& path : corpus_files(other)) {
        std::ifstream file(path, std::ios::binary);
        ASSERT_TRUE(file.good()) << path;
        const std::string text((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
        EXPECT_EQ(
            harness.fn(reinterpret_cast<const std::uint8_t*>(text.data()),
                       text.size()),
            0)
            << harness.name << " on " << path;
      }
    }
  }
}
