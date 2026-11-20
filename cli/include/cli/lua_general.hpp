#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sim/factions.hpp"
#include "sim/simulation.hpp"

namespace cli {

// One order a scripted general returned from decide(g): mapped onto the
// m11b player-order pipeline (the same issue_order the order scripts and
// replays use, with the same rate limits and delivery semantics).
struct GeneralOrder {
  bool squads_all = false;              // "all" instead of a squad list
  std::vector<std::uint32_t> squads;    // squad ids
  sim::PlayerOrderType type = sim::PlayerOrderType::Hold;
  bool has_point = false;               // x/y are meaningful
  double x = 0.0;
  double y = 0.0;
  std::string formation;                // ChangeFormation only
};

// A scripted AI general (M14 item 3). One instance per faction, each
// with its own sandboxed Lua 5.4 state: the script must define
//
//   function decide(g) ... return orders end
//
// where `g` is a fresh read-only snapshot table (see docs/generals.md)
// and `orders` is an array of
//   {squad = <id | "all">, order = "move_to" | "attack_move" | "hold" |
//    "retreat" | "regroup" | "formation", x = .., y = ..,
//    formation = ".."}.
// Only the base, math, string and table libraries exist — no io, os,
// package, debug or coroutine. Construction throws std::runtime_error
// (load error, or decide is not a function); decide() throws on script
// runtime errors and on malformed returned orders.
//
// Determinism: with no --general flags nothing below runs at all (every
// existing battle is untouched, byte-identical). With a general, the
// same script + seed + scenario reproduce the same orders and the same
// battle on a platform; scripts that call math.* transcendentals
// inherit the libm caveat documented in docs/milestone-14.md (item 4).
class LuaGeneral {
 public:
  explicit LuaGeneral(const std::string& source,
                      const std::string& name);
  ~LuaGeneral();
  LuaGeneral(const LuaGeneral&) = delete;
  LuaGeneral& operator=(const LuaGeneral&) = delete;
  LuaGeneral(LuaGeneral&&) noexcept;
  LuaGeneral& operator=(LuaGeneral&&) noexcept;

  // Snapshots `faction`'s view of the battle and runs decide(g).
  // Returns the orders (possibly empty).
  std::vector<GeneralOrder> decide(const sim::Simulation& simulation,
                                   sim::Faction faction);

  [[nodiscard]] const std::string& name() const noexcept { return name_; }

 private:
  void* state_ = nullptr;  // lua_State*, opaque so users need no lua.h
  std::string name_;
};

}  // namespace cli
