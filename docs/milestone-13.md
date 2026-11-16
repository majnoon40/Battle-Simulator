# Milestone 13 — combat depth, deterministic threads, UI completion, fuzzing, packaging

Working notes, one section per item, evidence first. Updated as items land.

## Item 0 — state check (report only)

### PRs and main

| PR | Title | State |
| --- | --- | --- |
| #4 | m11c — UI framework (widgets, layout, pure reducer, --ui-script harness) | merged |
| #5 | m11d — sandbox screens (menu, scenario picker, battle controls) | merged |
| #6 | m12 audit — ordered armies fight again; flat-map point orders; collapse ends orders; parser hardening | merged |

main is at the merge of #6 (sim version 11.2), all CI jobs green.

### The 11d spec versus what exists

| Feature | Status |
| --- | --- |
| UI framework: widgets, layout, pure reducer, DrawList drawing | done (m11c) |
| Menu / scenario picker / battle-control screens, App reducer | done (m11d) |
| `--ui-script` harness with `screen`/`scenarios` | done (m11d) |
| Window wiring: raylib input -> events -> reducer -> DrawList, screen switching, scenario reload | done in this milestone (item 1; the m11d patch was lost to a sandbox reset and re-derived) |
| Army editor with drag-to-place spawn | not done (item 5c) |
| Minimap (terrain thumbnail, unit dots, click to jump) | not done (item 5a) |
| Order UI (right-click MoveTo, AttackMove modifier, Hold/Retreat/Regroup hotkeys, order markers) | not done (item 5a) |
| End screen (result, curves, save replay/report, rematch) | not done (item 5b) |
| Replay viewer (list, scrub, verify) | not done (item 5b) |
| Settings (resolution, fullscreen, vsync, UI scale, colorblind palette) | not done (item 5c) |
| Scenario save/load through data-file validation | not done (item 5c) |

### Orders study re-run (final code, main 1993a90)

Five arms — none / AttackMove (all squads, enemy centroid at t150) /
flank (squads 0-2 north flank at t60, then AttackMove into the rear at
t400) / hold (all squads at t100) / retreat (all squads at t150) — 200
paired seeds x 2 orientations (the ordered faction is Red in both, so
side-averaging removes the spawn-band asymmetry), squads scenario, 1500
ticks (script: `scripts/m13_orders_study.py`; raw rows:
`download/m13/orders_study_200seeds.json`).

| arm | win share [95% CI] | mean end tick | kill ratio |
| --- | --- | --- | --- |
| none | 0.487 [0.439, 0.536] | 937.5 | 1.589 |
| attackmove | 0.560 [0.511, 0.608] | 913.9 | 2.141 |
| flank | 0.568 [0.519, 0.615] | 1015.3 | 1.712 |
| hold | 0.003 [0.000, 0.014] | 1462.8 | 0.046 |
| retreat | 0.003 [0.000, 0.014] | 1173.0 | 0.072 |

Paired per-seed deltas vs control: attackmove kill ratio +0.55
[+0.29, +0.82] (significant, favorable); flank +0.12 [-0.07, +0.31]
(win share significant, kill-ratio delta not); hold -1.54, retreat
-1.52 (both strongly unfavorable; the arms also stretch battles +525 /
+236 mean ticks). After the m12 audit the march orders are healthy
orders; the losing arms are battle dynamics (see the hold diagnosis)
plus the termination gap they expose.

### Hold diagnosis (why the kill ratio is ~0.05, with evidence)

Probe: `scripts/m13_hold_probe.cpp` replicates the hold arm (squads
scenario, all Red squads Hold at t100) and measures per tick, per unit,
whether an enemy is inside attack range and what the unit chose.

- **The order does not suppress fighting.** When a holding unit has an
  enemy inside attack range it chooses Attack 81.5% of engaged
  unit-ticks (seed 7; 86.3% seed 23); the rest is the cooldown dip
  (Attack briefly unavailable after each swing). The m12 fix holds.
- **The kill ratio comes from a rout cascade, not from refusing to
  fight.** Timeline (seed 7): first contact t~250; by t450 93/228 alive
  Red units are routed; by t550 193/216. The stationary blob's perimeter
  is focus-fired — melee targeting picks the nearest enemy, and a
  standing target stays the nearest for many attackers at once — so
  deaths concentrate in a small area, ally-death morale loss compounds
  (the blob keeps its dead nearby), and the rout wave rolls inward.
  Routed units cannot attack (forced Flee) and are run down: final 56
  Red dead vs 1 Blue (kill ratio 0.018 this seed; 0.042 seed 23; the
  study average is ~0.05).
- **Exposure is one-sided.** Standing (non-routed) Red accumulates only
  1071 engaged-unit-ticks across the whole battle (seed 7) while Blue's
  attackers accumulate 54,137: after the perimeter routs, the next
  standing units are engaged and routed within ticks, while every Blue
  attacker cycles through the front.
- For comparison, in the control run the exchange is a real fight: Red
  attack-when-engaged 32.1% vs Blue 35.2% (both cooldown-limited), kill
  ratio 1.38, and the loser's mass rout happens at the end, not in the
  first 200 ticks of contact.

Conclusion: "Hold on open ground loses" is battle dynamics (a
stationary, concentrated force is focus-fired and routed), not an order
bug. Items 2 (termination: a mass-routed faction ends the battle) and 3
(facing/flank damage: surrounding gets rewarded, but a disciplined line
with flanks secure fares better) both act on this evidence.

## Item 1 — window wiring (PR #7)

The m11d parked patch was lost to a sandbox reset; the wiring was
re-derived and, this time, the window-level policy is pure and tested
instead of living only in the unverified raylib file:

- `render/ui/shell.{hpp,cpp}` — the window policy: scenario listing
  (sorted .json stems), pointer-over-UI gating (`ui_covers`), arrow-key
  ownership (`ui_arrow_ownership`), momentary battle-button parsing
  (`battle_clicks`), the zoom slider mapping (`zoom_from_slider` /
  `slider_from_zoom`, log-symmetric around the fit zoom, exact inverses)
  and the per-frame viewer-state fold (`apply_battle_frame`).
- `game/src/main.cpp` — the wiring: raylib input -> `ui::Event` ->
  `reduce_app`; Quit exits; the picker's Start reloads the battle from
  `data/scenarios/<name>.json`; battle-screen buttons/checkboxes/slider
  fold into pause/speed/options/camera zoom; the wheel follows back
  into the slider; unit picking and wheel zoom are gated off UI panels;
  menus draw over a plain background exactly like the `--ui-script`
  harness. Compile-verified against a raylib-signature stub locally and
  by the CI game-build job; the human checklist is in docs/viewer.md.
- `tests/test_shell.cpp` — 8 tests (363 total): scenario listing, UI
  coverage on menu and battle screens, arrow ownership per screen and
  focus, click parsing, zoom round-trips, the viewer-state fold through
  real reducer clicks, and a menu -> picker -> battle session.

Known gaps (documented in docs/viewer.md, planned for 5a): no
battle->menu navigation in the App reducer yet; a window resize rebuilds
the screens without preserving widget values.
