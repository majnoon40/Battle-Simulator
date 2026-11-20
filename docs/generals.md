# Scripted AI generals

A *general* is a Lua 5.4 script that commands one faction: every
`--general-interval` ticks (default 30) it receives a fresh snapshot of
the battle and returns player orders, delivered through the same m11b
order pipeline as order scripts and replays — the same rate limits, the
same courier delays, the same rejection warnings.

## Using a general

```
battle_cli --scenario data/scenarios/three-way.json \
  --general red=aggressive.lua --general blue=defensive.lua
battle_cli --scenario data/scenarios/default.json --general mirror.lua
```

- `--general [faction=]script.lua`, repeatable. A bare path gives every
  faction its **own instance** of the same script (a self-play mirror);
  `red=gen.lua` binds one faction. A faction can be bound at most once.
- `--general-interval N` — decide every N ticks, in [1, 10000],
  default 30. The general runs *before* ticks N, 2N, 3N, …
- Generals need `--orders on` (the default) and one of the plain run
  paths; `--record`, `--replay`, the render modes, `--tactic-scan` and
  `--script` are rejected with `--general` (a replayed battle re-runs
  its generals live instead).

## The script

```lua
function decide(g)
  -- one order per own squad toward the enemy mass
  if g.enemies.count == 0 then return nil end
  return {
    {squad = "all", order = "attack_move",
     x = g.enemies.centroid_x, y = g.enemies.centroid_y},
  }
end
```

`decide` **must** be a global function. It receives a fresh snapshot
table `g` and returns `nil` (no orders) or an **array** of order
tables:

| field | type | meaning |
| --- | --- | --- |
| `squad` | id or `"all"` | the addressed squad (own faction's ids; `"all"` = every squad at issue time) |
| `order` | string | `"move_to"`, `"attack_move"`, `"hold"`, `"retreat"`, `"regroup"` or `"formation"` |
| `x`, `y` | numbers | optional, the point for point orders (both must be present) |
| `formation` | string | `"formation"` orders only, a formation id from `data/formations.json` |

Malformed orders fail the run with a clear error naming the order
index. Orders the pipeline rejects (unknown squad, rate limit, no
living commander) print a warning, exactly like order scripts.

## The snapshot

| field | value |
| --- | --- |
| `g.tick` | ticks completed so far (0 before the first) |
| `g.faction` / `g.faction_index` | the general's faction (`"red"`… / index) |
| `g.squads` | array — the general's own squads |
| `g.enemy_squads` | array — every other faction's squads |
| `g.enemies` | `{count, centroid_x, centroid_y}` over all living enemy units (the centroid fields are absent at count 0) |

Each squad table: `{id, x, y, members, commander, formation}` — the
squad id, the alive-members centroid, the number of living members,
whether command is active, and the current formation name.

The snapshot is a **god view**: the general sees the whole board, not
what its units can see. Restricting it to a fog-of-war view is future
work; the engine exposes nothing a script could not compute from the
export anyway.

## The sandbox

The Lua state opens **only** the base (`_G`), `math`, `string` and
`table` libraries. There is no `io`, `os`, `package`, `debug` or
`coroutine`, no file system, no process, no loader — a script cannot
load code or touch anything outside its battle. A script that errors
(including load errors) fails the run with the Lua error message.

## Determinism

Without `--general` nothing below runs at all — every battle is
byte-identical to the same command without the flags (pinned by the
whole existing suite). With a general, the same script + scenario +
seed reproduce the same orders and the same battle on a platform:
the snapshot is a pure function of the simulation state, Lua 5.4
arithmetic is IEEE-754 doubles, and the order pipeline is
deterministic. Scripts calling `math.sin`/`math.cos`/`math.atan`
inherit the libm caveat of `docs/milestone-14.md` (item 4): identical
on every platform we test, but not guaranteed by the language. The
determinism test (`LuaGeneralTest.ScriptedBattleIsDeterministic`)
re-runs a reactive script and compares every per-tick state hash.

## Testing generals

`tests/test_lua_general.cpp` covers the load errors, the sandbox (no
io/os/package/debug/coroutine globals), the snapshot contents, every
order-parsing shape, the malformed-order rejections, end-to-end
determinism, and that issued orders reach the delivery counters.
