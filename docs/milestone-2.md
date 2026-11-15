# Milestone 2 — Unit types and data-driven configuration

## Scope

Replace the hardcoded unit stats with JSON-defined unit types (infantry,
archer, cavalry), add scenario files (battlefield size, spawn band, per-faction
army composition), load them through the CLI (`--config`, `--scenario`), and
report per-type statistics in the CLI and JSON export. Same movement/combat
rules as M1; no AI, morale, squads, terrain or spatial hash. The 500-unit cap
and the O(n) neighbor scan stay.

## Design decisions and assumptions

1. **File formats** (documented in `data/README.md`). `data/units.json` is an
   array of `{id, health, damage, speed, range, cooldown}`; all values must be
   strictly positive (`cooldown` an integer ≥ 1), unknown fields are rejected,
   and ids must be unique and non-empty. `data/scenarios/*.json` carry
   `battlefield {width, height}`, `spawn_band_fraction` in (0, 0.5] and
   `armies {red, blue}` as non-empty `type -> count` objects; the scenario
   total may not exceed the 500-unit cap. Nothing is ever silently defaulted.
2. **infantry equals the M1 defaults** (health 100, damage 12, speed 2.0,
   range 2.0, cooldown 5) and `default.json` equals the M1 geometry
   (1000×1000, 15% band, 250+250). This makes the M1 regression exact.
3. **`--units N` is M1 mode**: an infantry-only split (Red gets the odd unit)
   using the loaded config's `infantry` type and the scenario's battlefield.
   With the shipped defaults it reproduces M1 bit for bit. `--units` cannot be
   combined with `--scenario` (the scenario defines the composition); the
   conflict is a parse-level error.
4. **Spawn order and RNG compatibility.** Red spawns first, then Blue; within
   an army, types spawn in scenario file order (parsed with
   `nlohmann::ordered_json` so file order is preserved). Every unit draws
   `x` then `y` from the seed's stream exactly like M1, so an infantry-only
   composition produces the identical battle as the M1 path — verified by
   test and by the CLI (default run == `--units 500` run, seed 42: Blue,
   456 ticks, 64 survivors).
5. **Movement/combat rules are unchanged from M1**, including units closing
   toward their target while attacking as soon as they are in range (M1 has
   no stop-at-range behavior; changing it would break the mandated M1
   regression). Archers therefore keep advancing after entering range, per
   M1 semantics; their advantage is the range window on approach.
6. **The 500-unit cap is now enforced** in `sim::validate_config` (both army
   modes), in scenario parsing and in the CLI (`--units` ≤ 500). M1
   documented the cap but never rejected larger values.
7. **`sim` stays I/O-free.** `parse_unit_types` / `parse_scenario` accept
   text or a `std::istream`; all file reading (and the "file does not exist"
   / "not valid JSON" messages with the path) lives in `cli/src/main.cpp`.
   The type table is referenced by `SimulationConfig::unit_types` as a
   non-owning pointer that must outlive the simulation.
8. **Per-type statistics** (`TypeReport`: spawned, survivors, kills,
   damage_dealt) follow M1 combat semantics: all applied damage counts
   (focus-fire overkill included, health clamped only on death), and a kill
   is credited once to the attack that brought the target from positive
   health to ≤ 0. Types that never spawned are omitted from reports.
9. **JSON export format v2**: per-unit `type`, `config.unit_types` +
   `config.armies` (composition mode) or the M1 `config.unit_stats` (legacy
   mode), and `statistics.<faction>.by_type`. Battles without a type table
   keep the M1 export shape. No timestamps, paths or host data anywhere.

## Verification results (2026-11-13)

- 109/109 GoogleTest tests pass (`ctest`, Release build; 69 M1 + 40 new).
- M1 regression: seed 42 `--units 500` → Blue, 456 ticks, Red 0 / Blue 64
  survivors; seed 123 → Blue, 713 ticks, 6 survivors; seed 5 → draw, 777
  ticks, 500 deaths; seed 0 → Blue, 709; seed 18446744073709551615 → Red,
  633. The default (composition) run matches the `--units 500` run exactly.
- Determinism: same-seed double runs produce byte-identical exports for
  seeds 42, 123, 0, 18446744073709551615 and for the mixed-type scenario.
- Mixed scenario (100 infantry, 100 archers, 50 cavalry per faction, seed
  42): Blue wins at tick 664, per-type stats in report and export; kill
  counts sum to the enemy's deaths on both sides.
- Editing `data/units.json` (archer range 8.0 → 12.0) changes the mixed
  battle (652 vs 664 ticks, 38 vs 22 Blue survivors) with no rebuild; the
  file was restored afterwards.
- Nine invalid-configuration demos fail with readable errors and exit 1
  (missing field, zero value, duplicate id, unknown scenario type, missing
  file, malformed JSON, over-cap scenario, `--units`+`--scenario` conflict,
  `--units` over cap) — `logs/m2-invalid-configs.log`.
- Purity: no `raylib`/`std::chrono`/`random_device`/`rand(` in `sim/`, no
  file I/O in `sim/`, no `time`/`date`/`path`/`host` values in the export.
- Performance (500 units, 2 vCPU): mixed scenario 7984 ticks/s,
  0.125 ms/tick, peak RSS 4.5 MiB; full-source rebuild ~27 s.

Full command logs live in `logs/m2-*.log`.
