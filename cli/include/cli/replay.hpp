#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sim/squads.hpp"

namespace cli {

// Replay file model + I/O (M10 Part 2b). The format is the line-based
// text format documented in sim/replay.hpp.
struct Intervention {
  std::uint32_t unit = 0;
  std::uint64_t tick = 0;  // the unit is dead by the end of this tick
};

// One timed player order from a --script file or a replay file (m11b).
// `squads_all` addresses every squad of the faction, else `squads`
// lists ids. Point orders carry (x, y); ChangeFormation carries
// `formation`.
struct OrderIntervention {
  std::uint64_t tick = 0;  // 1-based battle tick the order is issued on
  bool red = true;         // faction: true = Red, false = Blue
  bool squads_all = false;
  std::vector<std::uint32_t> squads;
  sim::PlayerOrderType type = sim::PlayerOrderType::Hold;
  double x = 0.0;
  double y = 0.0;
  bool has_point = false;
  std::string formation;
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
  // m11a: navigation generation ("v1" | "v2"). Files recorded before
  // m11a carry no nav flag; parse_replay defaults them to v1 so an old
  // file replays under the exact behavior it was recorded with.
  std::string nav = "v1";
  // m11b: the orders side of the flags line ("on" | "off"); files from
  // before m11b parse as "on" (orders disabled was not representable).
  std::string orders = "on";
  std::vector<Intervention> interventions;
  // m11b: the timed player orders applied to the run (recorded on
  // --record, re-applied by --replay). Ascending tick, then file order.
  std::vector<OrderIntervention> order_interventions;
  std::vector<std::pair<std::uint64_t, std::string>> hashes;
};

// Parses a --script orders.json file (m11b): a JSON array of
// {"tick": n, "faction": "red"|"blue", "squads": [ids]|"all",
//  "order": "moveto"|"attackmove"|"hold"|"retreat"|"regroup"|"formation",
//  "x": n, "y": n, "formation": "id"} objects. moveto/attackmove require
// x and y; retreat takes them optionally; formation requires formation.
// On failure returns false and fills `error`.
[[nodiscard]] bool parse_order_script(const std::string& path,
                                      std::vector<OrderIntervention>& out,
                                      std::string& error);

// Parses a replay file. On failure returns false and fills `error`
// with a clear message (unknown magic, missing fields, bad values,
// non-ascending checkpoints...).
[[nodiscard]] bool parse_replay(const std::string& path, ReplayData& out,
                                std::string& error);

// Writes a replay file (header + interventions + orders + checkpoints).
[[nodiscard]] bool write_replay(const std::string& path,
                                const ReplayData& data);

}  // namespace cli
