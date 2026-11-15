#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cli {

// Replay file model + I/O (M10 Part 2b). The format is the line-based
// text format documented in sim/replay.hpp.
struct Intervention {
  std::uint32_t unit = 0;
  std::uint64_t tick = 0;  // the unit is dead by the end of this tick
};

struct ReplayData {
  std::string version;
  std::string scenario;
  std::string mod;  // "-" = none
  std::uint64_t seed = 0;
  std::uint64_t ticks = 0;
  std::string variance = "on";
  std::string contact_seek = "on";
  std::string neighbor = "grid";
  std::string squads = "on";
  std::string swap_sides = "off";
  std::vector<Intervention> interventions;
  std::vector<std::pair<std::uint64_t, std::string>> hashes;
};

// Parses a replay file. On failure returns false and fills `error`
// with a clear message (unknown magic, missing fields, bad values,
// non-ascending checkpoints...).
[[nodiscard]] bool parse_replay(const std::string& path, ReplayData& out,
                                std::string& error);

// Writes a replay file (header + interventions + checkpoints).
[[nodiscard]] bool write_replay(const std::string& path,
                                const ReplayData& data);

}  // namespace cli
