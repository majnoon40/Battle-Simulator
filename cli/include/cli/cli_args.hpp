#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <ostream>
#include <string>

#include "sim/config.hpp"

namespace cli {

struct Arguments {
  bool help = false;
  std::uint64_t seed = 42;
  std::uint32_t units = 500;  // total across both factions; Red gets the odd extra unit
  bool units_explicit = false;  // --units was passed: infantry-only split (M1 mode)
  std::uint64_t ticks = 3000;
  std::string config_path;    // unit type table; empty -> data/units.json
  std::string scenario_path;  // battle scenario; empty -> data/scenarios/default.json
  bool grid_mode = true;      // --neighbor-mode grid (default) vs brute
  bool neighbor_mode_explicit = false;
  double grid_cell_size = 16.0;  // --grid-cell; positive
  bool grid_cell_explicit = false;
  bool utility_ai = true;     // --ai-mode utility (default) vs legacy
  bool ai_mode_explicit = false;
  std::string personalities_path;  // empty -> data/personalities.json
  bool morale_on = true;      // --morale on (default) vs off; utility mode only
  bool morale_explicit = false;
  std::string morale_config_path;   // empty -> data/morale.json
  bool squads_on = true;       // --squads on (default) vs off; utility mode only
  bool squads_explicit = false;
  std::string formations_path;  // empty -> data/formations.json
  std::string commanders_path;  // empty -> data/commanders.json
  // M10 tactical depth: --variance on (default) enables combat variance,
  // decision intervals, memory and Search/Help/Flank; off reproduces
  // M1-M9 byte-for-byte. --swap-sides swaps spawn bands, objectives and
  // compositions. --tactics overrides the tactics config path.
  bool variance_on = true;
  bool variance_explicit = false;
  bool contact_seek_on = true;  // --contact-seek on (default) vs off
  bool flee_clamp_on = true;    // --flee-clamp on (default) vs off
  bool swap_sides = false;
  // m11a navigation generation: --nav v2 (default; fixed explicit-objective
  // swap under --swap-sides, terrain-aware formation march) or v1 (the
  // exact M10 navigation — byte-identical to the M10 binary, including
  // the swapped-orientation stall it had).
  bool nav_v2 = true;

  // m13 item 2: availability (--ai-avail v2|v1) and termination
  // (--end v2|v1) rules; v2 default, v1 reproduces milestone-12.
  bool ai_avail_v2 = true;
  bool end_v2 = true;

  // m13 item 3: combat depth (--combat v2|v1); v2 default, v1
  // reproduces milestone-12 combat byte-for-byte.
  bool combat_v2 = true;

  // m13 item 4: deterministic multithreading (--threads N; 0 = use
  // hardware concurrency, resolved in main with a printed note; default
  // 1 = all phases inline, byte-identical to the pre-threads binary).
  // Output is byte-identical for every thread count. --profile-phases
  // prints per-phase timing (perception / decision / resolution /
  // commit / total) after the run; profiling only reads the clock.
  std::uint32_t threads = 1;
  bool threads_explicit = false;
  bool profile_phases = false;
  // M15 S1: --check-invariants[=N] — validate the simulation state
  // every N ticks (N defaults to 1, every tick, when the flag is bare).
  // Off by default: battles are byte-identical without the flag, and a
  // healthy battle is byte-identical with it (the checks only read
  // state). On the first violation the CLI prints the tick, unit,
  // invariant and a state dump, writes a replay of the run and exits 3.
  bool invariants_on = false;
  std::uint32_t invariants_interval = 1;
  // m11b player orders: --orders on (default) or off (issue_order is
  // inert; a --script is rejected). With no order issued battles are
  // byte-identical either way.
  bool orders_on = true;
  // m11b: --script orders.json — timed player orders for headless runs
  // (JSON array; see cli/replay.hpp parse_order_script for the shape).
  // Empty = none. Orders are recorded into --record replays and
  // re-applied by --replay.
  std::string script_path;
  std::string tactics_path;  // empty -> data/tactics.json
  // M10: --tactic-scan <action> runs the battle tick by tick and arms the
  // AI debug capture on the first unit that chooses that action (search,
  // help or flank), then prints its decision block.
  std::string tactic_scan;  // empty = off
  std::uint32_t ai_debug_unit = 0;  // --ai-debug <unit id>; 0 = off
  std::uint64_t ai_debug_tick = 0;  // --ai-debug-tick <n>; 0 = first enemy seen
  bool ai_debug_tick_explicit = false;
  std::int64_t squad_debug_id = -1;  // --squad-debug <squad id>; -1 = off
  // Mod overlay (M8): --mod <dir> replaces whole data files of the same
  // name (units/terrain/personalities/morale/formations/commanders, plus
  // scenarios/ and maps/ subdirectories). No partial merging; an invalid
  // mod is rejected wholesale with readable errors.
  std::string mod_dir;  // empty = no mod
  // Headless rendering (M7): --render-frame <tick> --out <file.png> writes
  // one PNG (camera fit to the world, 1280x720, the same DrawList code the
  // window uses); --render-frames a,b,c --out-prefix <dir/name> writes one
  // <prefix>-<tick:6>.png per tick; --ascii <tick> prints a compact
  // terminal map. In render/ascii modes the simulation stops right after
  // the last target tick.
  std::uint64_t render_frame = 0;  // 0 = off
  std::string render_frames;       // empty = off; comma-separated ticks
  std::string render_out;          // --out (single frame)
  std::string render_out_prefix;   // --out-prefix (frame list)
  std::uint64_t ascii_tick = 0;    // 0 = off
  // m11c: --ui-script script.json — headless UI harness. The script is
  // a JSON object {width, height, tick?, out, events: [{frame, type:
  // key|move|down|up|wheel, key?, x?, y?, button?, delta?}]}; the demo
  // control panel is built at the given size over a battle frame at
  // `tick`, every event is reduced through the pure reducer, one PNG
  // per frame is written (out pattern, %d = frame) and a per-frame
  // state line is printed to stdout. Mutually exclusive with the
  // render/ascii modes.
  std::string ui_script;
  bool export_json = false;
  std::string export_path;
  // M10 Part 2a: --events out.jsonl (battle event log), --export-csv
  // out.csv (timeline every csv_interval ticks, default 10), --report
  // out.md (markdown + .json twin), --report-png out.png (casualty and
  // morale curves via the software rasterizer).
  bool events_on = false;
  std::string events_path;
  bool export_csv = false;
  std::string csv_path;
  std::uint32_t csv_interval = 10;
  bool report_on = false;
  std::string report_path;
  bool report_png_on = false;
  std::string report_png_path;
  // M10 Part 2b: replay recording and verification.
  bool record_on = false;
  std::string record_path;
  bool replay_on = false;
  std::string replay_path;
  bool replay_verify = false;  // --replay PATH --verify
  std::uint64_t replay_seek = 0;  // --seek T: re-run to tick T (0 = off)
  std::vector<std::string> interventions;  // --intervene kill:UNIT:TICK
  // M14 item 3: scripted AI generals. Each entry is
  // "[faction=]script.lua" — a bare path gives every faction its own
  // instance of the same script; "red=gen.lua" binds one faction.
  std::vector<std::string> generals;
  std::uint32_t general_interval = 30;  // --general-interval ticks
  // m11a: --trace out.jsonl writes one navigation-trace line per
  // --trace-interval ticks (per-faction centroid, mean move, still
  // fraction, Advance fraction, x histogram; see cli/trace.hpp). Pure
  // observation: a traced battle is bit-identical to an untraced one.
  bool trace_on = false;
  std::string trace_path;
  std::uint32_t trace_interval = 50;
};

struct ParseOutcome {
  bool ok = false;
  std::string error;  // human-readable message when ok is false
  Arguments args;
};

// Parses argv. On failure, `error` holds a descriptive message. `--help`
// short-circuits and succeeds with args.help = true.
[[nodiscard]] ParseOutcome parse_args(int argc, char** argv);

void print_usage(std::ostream& out);

// m11a: the --swap-sides setup mirror, extracted from main so it is
// unit-testable. Swaps compositions, squad plans, spawn patterns and —
// exactly once — the explicit objectives between the factions (the M10
// version swapped them per faction in a loop, which swapped them right
// back: a swapped battle parked each army on the objective inside its
// own spawn band; see docs/terrain.md). No-op without swap_sides.
void apply_side_swap(sim::SimulationConfig& config);

}  // namespace cli
