// M14 item 3: scripted AI generals. The Lua host, the read-only battle
// snapshot handed to decide(g), and the parser for the returned orders.
// The Lua state is created per faction and only ever sees the sandbox
// libraries (see lua_general.hpp); scripts issue orders through the
// same m11b pipeline as order scripts and replays.
#include "cli/lua_general.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

// The lua.org sources ship no extern "C" guards, so the declarations
// must be wrapped for this C++ translation unit (lua_static is built
// as C; without the wrap the symbols get mangled and fail to link).
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "sim/squads.hpp"

namespace cli {
namespace {

// Registers exactly the sandbox libraries. io, os, package, debug and
// coroutine are never registered — their code is compiled but no global
// or field ever reaches the script.
void open_sandbox_libs(lua_State* state) {
  luaL_requiref(state, "_G", luaopen_base, 1);
  lua_pop(state, 1);
  luaL_requiref(state, "math", luaopen_math, 1);
  lua_pop(state, 1);
  luaL_requiref(state, "string", luaopen_string, 1);
  lua_pop(state, 1);
  luaL_requiref(state, "table", luaopen_table, 1);
  lua_pop(state, 1);
}

[[noreturn]] void fail(const std::string& name, const std::string& what) {
  throw std::runtime_error("general '" + name + "': " + what);
}

void number_field(lua_State* state, const char* key, double value) {
  lua_pushstring(state, key);
  lua_pushnumber(state, value);
  lua_settable(state, -3);
}

void integer_field(lua_State* state, const char* key, lua_Integer value) {
  lua_pushstring(state, key);
  lua_pushinteger(state, value);
  lua_settable(state, -3);
}

void string_field(lua_State* state, const char* key, const char* value) {
  lua_pushstring(state, key);
  lua_pushstring(state, value);
  lua_settable(state, -3);
}

void boolean_field(lua_State* state, const char* key, bool value) {
  lua_pushstring(state, key);
  lua_pushboolean(state, value ? 1 : 0);
  lua_settable(state, -3);
}

// One squad table: {id, x, y, members, commander, formation}.
void push_squad(lua_State* state, const sim::Simulation& simulation,
                const sim::Squad& squad) {
  std::uint32_t alive = 0;
  for (const sim::SquadMemberState& member : squad.members) {
    if (simulation.world().units[member.unit_index].alive) {
      ++alive;
    }
  }
  lua_newtable(state);
  integer_field(state, "id", squad.id);
  number_field(state, "x", squad.centroid_x);
  number_field(state, "y", squad.centroid_y);
  integer_field(state, "members", alive);
  boolean_field(state, "commander", squad.command_active);
  string_field(state, "formation",
               sim::Simulation::formation_name(squad.formation).c_str());
}

}  // namespace

LuaGeneral::LuaGeneral(const std::string& source, const std::string& name)
    : name_(name) {
  lua_State* state = luaL_newstate();
  if (state == nullptr) {
    fail(name, "cannot create the Lua state (out of memory)");
  }
  open_sandbox_libs(state);
  if (luaL_loadbuffer(state, source.data(), source.size(), name.c_str()) !=
          LUA_OK ||
      lua_pcall(state, 0, 0, 0) != LUA_OK) {
    const char* message = lua_tostring(state, -1);
    const std::string error = message != nullptr ? message : "unknown error";
    lua_close(state);
    fail(name, "load error: " + error);
  }
  lua_getglobal(state, "decide");
  if (!lua_isfunction(state, -1)) {
    lua_close(state);
    fail(name, "must define decide(g)");
  }
  lua_pop(state, 1);
  state_ = state;
}

LuaGeneral::~LuaGeneral() {
  if (state_ != nullptr) {
    lua_close(static_cast<lua_State*>(state_));
    state_ = nullptr;
  }
}

LuaGeneral::LuaGeneral(LuaGeneral&& other) noexcept
    : state_(other.state_), name_(std::move(other.name_)) {
  other.state_ = nullptr;
}

LuaGeneral& LuaGeneral::operator=(LuaGeneral&& other) noexcept {
  if (this != &other) {
    if (state_ != nullptr) {
      lua_close(static_cast<lua_State*>(state_));
    }
    state_ = other.state_;
    name_ = std::move(other.name_);
    other.state_ = nullptr;
  }
  return *this;
}

std::vector<GeneralOrder> LuaGeneral::decide(
    const sim::Simulation& simulation, sim::Faction faction) {
  lua_State* state = static_cast<lua_State*>(state_);
  const std::size_t index = static_cast<std::size_t>(faction);

  // ---- the snapshot table `g` --------------------------------------
  lua_newtable(state);
  integer_field(state, "tick", static_cast<lua_Integer>(
                                  simulation.world().tick_count));
  string_field(state, "faction",
               std::string(sim::faction_key(faction)).c_str());
  integer_field(state, "faction_index", static_cast<lua_Integer>(index));

  // Own squads and enemy squads. The general sees the whole board (the
  // god view, documented in docs/generals.md); fog-of-war snapshots
  // would filter this, not add engine state.
  const char* const squad_sets[2] = {"squads", "enemy_squads"};
  for (int own = 0; own < 2; ++own) {
    lua_pushstring(state, squad_sets[own]);
    lua_newtable(state);
    lua_Integer entry = 1;
    for (const sim::Squad& squad : simulation.squads()) {
      const bool is_own = squad.faction == faction;
      if (is_own != (own == 0)) {
        continue;
      }
      lua_pushinteger(state, entry++);
      push_squad(state, simulation, squad);
      lua_settable(state, -3);
    }
    lua_settable(state, -3);
  }

  // Aggregate enemy strength: every living unit of every other faction.
  {
    std::uint32_t count = 0;
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (const sim::Unit& unit : simulation.world().units) {
      if (!unit.alive || unit.faction == faction) {
        continue;
      }
      ++count;
      sum_x += unit.x;
      sum_y += unit.y;
    }
    lua_pushstring(state, "enemies");
    lua_newtable(state);
    integer_field(state, "count", count);
    if (count > 0) {
      number_field(state, "centroid_x", sum_x / count);
      number_field(state, "centroid_y", sum_y / count);
    }
    lua_settable(state, -3);
  }

  // ---- decide(g) ----------------------------------------------------
  lua_getglobal(state, "decide");
  lua_insert(state, -2);  // decide below the snapshot
  if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
    const char* message = lua_tostring(state, -1);
    const std::string error = message != nullptr ? message : "unknown error";
    lua_pop(state, 1);
    fail(name_, "decide failed: " + error);
  }

  // ---- the returned orders ------------------------------------------
  std::vector<GeneralOrder> orders;
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return orders;  // no orders this cycle
  }
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    fail(name_, "decide must return nil or an array of orders");
  }
  const lua_Integer count = luaL_len(state, -1);
  for (lua_Integer i = 1; i <= count; ++i) {
    const std::string where =
        "order " + std::to_string(static_cast<int>(i));
    lua_geti(state, -1, i);
    if (!lua_istable(state, -1)) {
      lua_pop(state, 2);
      fail(name_, where + " is not a table");
    }
    GeneralOrder order;
    // squad: "all" or a squad id.
    lua_getfield(state, -1, "squad");
    if (lua_isstring(state, -1) != 0 &&
        std::string(lua_tostring(state, -1)) == "all") {
      order.squads_all = true;
    } else if (lua_isinteger(state, -1) != 0) {
      order.squads.push_back(static_cast<std::uint32_t>(
          lua_tointeger(state, -1)));
    } else {
      lua_pop(state, 3);
      fail(name_, where + " needs squad = <id | \"all\">");
    }
    lua_pop(state, 1);
    // order: one of the player order types.
    lua_getfield(state, -1, "order");
    if (lua_isstring(state, -1) == 0) {
      lua_pop(state, 3);
      fail(name_, where + " needs order = \"move_to\" | \"attack_move\" | "
                        "\"hold\" | \"retreat\" | \"regroup\" | \"formation\"");
    }
    if (!sim::parse_player_order_type(lua_tostring(state, -1),
                                       order.type)) {
      const std::string type = lua_tostring(state, -1);
      lua_pop(state, 3);
      fail(name_, where + " has unknown order '" + type + "'");
    }
    lua_pop(state, 1);
    // x / y: optional pair (MoveTo / AttackMove / Retreat points).
    bool has_x = false;
    bool has_y = false;
    lua_getfield(state, -1, "x");
    if (lua_isnumber(state, -1) != 0) {
      order.x = lua_tonumber(state, -1);
      has_x = true;
    }
    lua_pop(state, 1);
    lua_getfield(state, -1, "y");
    if (lua_isnumber(state, -1) != 0) {
      order.y = lua_tonumber(state, -1);
      has_y = true;
    }
    lua_pop(state, 1);
    order.has_point = has_x && has_y;
    // formation: ChangeFormation's target id.
    lua_getfield(state, -1, "formation");
    if (lua_isstring(state, -1) != 0) {
      order.formation = lua_tostring(state, -1);
    }
    lua_pop(state, 1);
    lua_pop(state, 1);  // the order table
    orders.push_back(std::move(order));
  }
  lua_pop(state, 1);  // the returned table
  return orders;
}

}  // namespace cli
