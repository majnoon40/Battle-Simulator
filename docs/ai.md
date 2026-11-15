# Utility AI (M4)

How a utility-mode unit perceives, scores and chooses actions. The legacy
M1-M3 AI (unlimited sight, attack whenever in range) remains available via
`--ai-mode legacy` and reproduces every reference result exactly.

## Perception

- Per unit per tick, from the tick-start snapshot only: all enemies and
  allies within `sight_radius` (inclusive), found with spatial grid radius
  queries — never all-pairs scans. A unit exactly at the sight radius is
  seen; one just beyond it is not.
- Outputs: nearest visible enemy (minimum squared distance, exact ties to
  the lower unit id — the M1 targeting rule inside the sight limit), the
  visible enemy and ally counts (allies exclude the unit itself), and the
  visible enemy centroid.
- Dead units are never seen or targeted (the grid removes them on death and
  queries re-check liveness).

## Personalities

`data/personalities.json` defines archetypes — brave, cautious, aggressive,
balanced — each with a `[min, max]` range per trait (bravery, caution,
aggression, all within [0, 1]) and a spawn weight. A unit's personality is
drawn from a SplitMix64 hash of `(seed, unit id)`: the hash seeds a private
generator that picks the archetype by weighted draw and then draws each
trait uniformly from its range, in the order bravery, caution, aggression.
The spawn RNG stream is never touched, so spawn positions are identical in
legacy and utility modes, and the same (seed, id, table) always yields the
same personality on every platform. Tests may inject personalities
explicitly via the `Simulation` constructor.

## Objectives

Each faction has one objective point — the only global information units
have. It defaults to the center of the opposing faction's spawn band (Red
advances toward Blue's side and vice versa) and can be overridden per
faction in the scenario file (`"objectives": {"red": {"x": .., "y": ..}}`);
overridden points must lie on the battlefield.

## Score inputs (all clamped to [0, 1])

| Input | Definition |
| --- | --- |
| `health` | health / max_health |
| `strength` | (visible allies + 1) / (visible allies + visible enemies + 1) |
| `distance` | distance to nearest visible enemy / sight radius |
| `readiness` | 1 − cooldown_remaining / attack_cooldown_ticks |
| `bravery`, `caution`, `aggression` | personality traits |

## Action scores

Every score is a fixed linear blend; the weights of one action sum to
exactly 1.0, so a score can never leave [0, 1] or become non-finite.
Unavailable actions score −1.0.

| Action | Available when | Score |
| --- | --- | --- |
| Attack | a visible enemy is within attack range | `0.25 + 0.20·health + 0.20·strength + 0.15·readiness + 0.20·bravery` |
| Approach | at least one visible enemy | `0.20 + 0.25·aggression + 0.20·strength + 0.15·health + 0.20·distance` |
| Advance | no visible enemy | `0.50 + 0.30·aggression + 0.20·health` |
| Retreat | at least one visible enemy | `0.10 + 0.35·(1−health) + 0.20·(1−strength) + 0.35·caution` |
| Hold | always | `0.15 + 0.30·caution + 0.25·(1−health) + 0.30·(1−readiness)` |

Rationale: Attack rewards health, local superiority and a ready weapon, and
bravery keeps low-health units fighting. Approach rises with aggression and
with distance (a far enemy cannot be attacked, so closing is the productive
move). Advance dominates the no-enemy case (0.50 base) so armies keep
moving, but wounded cautious units prefer Hold. Retreat grows as health and
local strength fall and with caution; only extreme cowards (caution far
above aggression) flee a full-health, even duel.

## Choice, stickiness and commit time

The highest score wins; exact ties break by the fixed order
Attack > Approach > Advance > Retreat > Hold. Two anti-flip-flop mechanisms:

1. **Stickiness bonus** (`SimulationConfig::action_stickiness`, default
   0.05): added to the current action's score before comparing.
2. **Minimum commit time** (`kActionCommitTicks = 3`): after switching, a
   unit keeps its new action for three ticks before scores are consulted
   again, as long as the action stays available. This damps score cycles
   such as the cooldown-readiness dip right after an attack.

Measured on the standard battle (500 infantry, seed 42): **2.08 action
switches per unit per 100 ticks**; the documented acceptance bound is 8.

## Movement and combat per action

- **Attack**: stop; the combat phase attacks if the target is still alive
  and in range after movement and the cooldown is ready. Archers therefore
  hold at their own range (measured: average attack distance 5.0 for
  archers vs 0.9 for infantry in the mixed scenario).
- **Approach**: move toward the nearest visible enemy (`min(speed, dist)`).
- **Advance**: move toward the faction objective (`min(speed, dist)`).
- **Retreat**: move away from the visible enemy centroid with the horizontal
  component forced toward the unit's own side, clamped to the battlefield,
  at **half speed** — a fighting withdrawal, so equal-speed pursuers still
  close instead of an eternal kite. A unit exactly at the centroid falls
  back straight toward its own side.
- **Hold**: no movement.

Only the Attack action engages in combat (utility mode); Hold, Retreat,
Advance and Approach units do not attack that tick.

## Debug and export

`--ai-debug <unit_id> [--ai-debug-tick N]` prints the unit's health,
personality, visible counts, all five action scores and the selected action
for the given tick, or for the first tick it sees an enemy. The JSON export
(format v3) adds per-unit `personality` and `last_action` plus per-faction
cumulative `action_counts`; legacy battles export `last_action: "none"` and
zero counts.

## Known behavior notes

- Utility battles are wars of attrition: wounded cautious units disengage,
  lose sight, advance again and re-engage, so 500-unit battles typically
  run to the tick limit with about half the force lost rather than ending
  in annihilation. A morale system (a later milestone) is the natural fix.
- Utility-mode perception always uses the spatial grid, so `--neighbor-mode`
  has no effect on utility battles (verified byte-identical).

# Tactical depth (M10)

M10 adds combat variance, decision intervals, personal memory with squad
intel sharing, and three new scored actions — Search, Help and Flank —
plus bounded pursuit of routed enemies and a regroup score term. All of
it is active with `--variance on` (the default, utility mode only) and
completely absent with `--variance off`, which reproduces every M1-M9
reference byte-for-byte (the M1-M9 battle has no battle-time randomness
at all; exports stay format 3-6 with the five-action set, format 7
carries the eight-action set).

## Counter-based randomness and channels

With variance on, every roll comes from the pure function
`tactics_roll(seed, unit_id, tick, channel)` (SplitMix64 mixing) — a
counter-based RNG with no shared stream, so no result can depend on
iteration order and shuffling the unit vector cannot change any roll.
The spawn RNG stream (uniform positions in the Band pattern) is
untouched. Channels:

| channel | used for |
| --- | --- |
| 0 | combat hit roll (compared against the type's accuracy) |
| 1 | combat damage roll (damage variance spread) |
| 2 | cooldown jitter roll (post-attack cooldown spread) |
| 3 | decision interval draw (spawn-time, 1-5 ticks) |
| 4 | composition range draw (scenario `[min, max]` army counts) |
| 5 | spawn pattern jitter (cluster centers and offsets) |

Per-type variance fields (data/units.json, optional, validated in
[0, 1]; absent = the deterministic M1-M9 values): `accuracy` (per-attack
hit chance), `damage_variance` (damage multiplied by
`1 + v*(2*roll-1)`), `cooldown_jitter` (cooldown multiplied the same
way, at least 1 tick). A miss still costs the attack cooldown.

## Decision intervals

Each unit re-scores its actions every 1-5 ticks (drawn once at spawn
from channel 3, like the personality). Between decisions the committed
action persists exactly like the M4 commit time, which the interval
replaces when tactics are on. Units therefore react with a
personality-staggered delay instead of in lockstep.

## Memory and squad intel (data/tactics.json)

A unit that sees an enemy remembers its nearest visible enemy's position
(the tick it was seen on). After `memory.forget_ticks` ticks (default
1200, measured: remnants of equal-army battles outlive shorter memories and
the battle re-stalls) the memory is gone. Squad members inside the acting commander's
leadership radius additionally publish that position into a shared
contact list (the freshest `memory.squad_intel_max_contacts` = 8
entries, newest first); a member may read a contact only
`memory.squad_intel_delay_ticks` ticks (default 12) after it was seen —
intel, like orders, travels with a delay. A unit with no visible enemy
may act on its own memory or the freshest readable squad contact; after
forgetting it falls back to Advance.

## New actions (scores in [0, 1], weights sum to 1.0, ties by the fixed
enum order Attack > Approach > Advance > Retreat > Hold > Search > Help
> Flank)

- **Search** — available with no visible enemy and a contact fresher
  than forget_ticks. While such a contact exists, Advance is NOT
  available (memory first; Advance is the fallback after forgetting —
  the M10 spec's semantics). Score:
  `0.55 + 0.25*(1 - clamp(contact_distance/sight, 0, 1)) + 0.20*(1 - clamp(age/forget_ticks, 0, 1))`.
  Movement: straight toward the contact. Search carries the commander's
  Advance order bonus (searching toward reported contact IS advancing).
- **Help** — available when a visible ally inside `help.radius`
  (default 40) is in need (health fraction < 0.5 or damaged on the
  previous tick) and the helper's local strength share is at least
  `help.strength_ratio_min` (0.8):
  `0.50 + 0.30*(1 - ally_health_fraction) + 0.20*(1 - clamp(ally_distance/radius, 0, 1))`.
  Movement: toward the neediest such ally (lowest health, ties to the
  lower unit id).
- **Flank** — available when the unit's squad had a member seeing an
  enemy on the previous tick (frontally engaged), the local ratio
  (visible allies + 1) / (visible enemies + 1) is at least
  `flank.outnumber_ratio` (1.25), and the member is under the flanker
  cap (the first `flank.max_per_squad` = 4 soldiers in member order;
  the commander never flanks):
  `0.65 + 0.20 * clamp(strength_ratio, 0, 1)`.
  Movement: toward a point `flank.offset_distance` (25) world units
  perpendicular to the squad-centroid-to-enemy-centroid line (the side
  fixed by member index parity); once there the next decision naturally
  shifts to Approach/Attack.

## Score terms on existing actions

- **Pursuit of routed enemies**: when the nearest visible enemy is
  routed, Approach gains
  `pursuit.routed_bonus (0.25) * (speed >= 4.0 ? 2.0 : 1.0)` — cavalry
  pursues harder. The bonus is bounded by BOTH
  `pursuit.max_ticks` (240 consecutive pursuit ticks) and
  `pursuit.max_distance` (300 world units from the pursuit's anchor).
  Past either bound the pursuer disengages: the routed enemy no longer
  drives Approach (an adjacent one can still be attacked) and the
  unit's memory is suppressed that tick, so winners never chase forever.
  Switching targets resets the counter.
- **Regroup**: with no enemy visible and squad cohesion below
  `regroup.cohesion_below` (0.35), Advance gains
  `regroup.bonus_max * (1 - cohesion)` (max 0.20) — whose movement
  already steers members back to their formation slots.

## Contact seeking (M10 continuation, --contact-seek on|off, default on)

Measured problem on the default 250v250 band-spawn scenario (diagnosis
over 20 seeds, logs in `logs/`): first contact is reliable (tick
167-173), but by tick 400 the armies have already passed through each
other (0% of units have any enemy within the 40-unit sight radius;
army centroids 450+ apart) and from tick ~600 they park near the
opposing objectives while the gap widens to 750-800 by tick 2500.
Personal memory cannot bridge this: it expires while the armies are
parked, and without squads there is no intel sharing at all.

The fix (one deterministic mechanism, no randomness): a **faction-level
enemy estimate** — the mean position of the nearest visible enemy of
every member that sees one, updated each tick any member sees an enemy
(delayed by nature: it is the last sighting; noisy: only seen enemies
contribute). A unit that has seen no enemy for
`contact_seek.idle_ticks` (100) consecutive ticks escalates its
freshest contact to that estimate (while it is at most
`contact_seek.estimate_max_age` (2400) ticks old): Search then heads
for where the enemy was last seen by anyone instead of standing on a
stale personal memory. Since both factions' estimates point into the
contact zone, armies that passed each other converge back on it and
re-engage; any new sighting refreshes the estimates, so the seek
tightens as the sides close. Scores are unchanged — only the Search
goal moves. `--contact-seek off` (or `--variance off`) disables the
whole mechanism: off is byte-identical to the Part 1 binary (verified
by cmp), off-off to M9.

## Spawn patterns and composition ranges (scenario opt-in)

`"spawn_pattern": "line" | "columns" | "echelon" | "cluster"` (or
per-faction `"spawn_patterns": {"red": ..., "blue": ...}`) replaces the
default uniform random Band placement with deterministic formation math
inside the faction's band (cluster jitter from channel 5). Army counts
may be written as `[min, max]` ranges; the count is drawn per seed from
channel 4, so the same seed always fields the same army. Existing
scenarios keep their exact behavior unless they opt in.

## Side swapping and balance

`--swap-sides` mirrors the whole setup: Red deploys in the eastern band,
compositions, explicit objectives, squad plans and spawn patterns swap
with it (unit factions are untouched). `tools/analysis/balance.cpp`
(build: `g++ -std=c++20 -O2 tools/analysis/balance.cpp -o build/balance`)
runs a scenario for N seeds in both orientations and reports the
per-orientation split, the side-averaged win rate and the orientation
effect.
