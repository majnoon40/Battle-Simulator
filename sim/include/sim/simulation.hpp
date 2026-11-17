#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "sim/ai.hpp"
#include "sim/config.hpp"
#include "sim/flow_field.hpp"
#include "sim/personalities.hpp"
#include "sim/results.hpp"
#include "sim/rng.hpp"
#include "sim/events.hpp"
#include "sim/spatial_hash.hpp"
#include "sim/timeline.hpp"
#include "sim/squads.hpp"
#include "sim/tactics.hpp"
#include "sim/world.hpp"

namespace sim {

// A unit's squad view (M6), for exports, the CLI and tests: which squad it
// belongs to (-1 when squadless), its effective order, the squad's formation
// and the squad's current cohesion.
struct UnitSquadInfo {
  std::int32_t squad_id = -1;
  Order order = Order::None;
  FormationKind formation = FormationKind::Line;
  double cohesion = 0.0;
};

// AI debug capture (M4): the CLI arms this before run() and the simulation
// fills it in during the decide phase of the matching tick — either the
// explicitly requested tick, or the first tick on which the unit sees at
// least one enemy. Pure data capture; the simulation performs no I/O.
struct AiDebugCapture {
  // Request (set by the caller before the battle).
  std::uint32_t unit_id = 0;   // 0 = inactive
  std::uint64_t tick = 0;      // 1-based tick number to capture
  bool explicit_tick = false;  // false: first tick the unit sees an enemy
  // Result (filled by the simulation).
  bool captured = false;
  bool unit_found = false;
  std::uint64_t captured_tick = 0;
  double health = 0.0;
  double max_health = 0.0;
  std::uint32_t visible_enemies = 0;
  std::uint32_t visible_allies = 0;
  int target = -1;  // index of the nearest visible enemy, -1 when none
  double target_distance = 0.0;
  Personality personality{};
  ActionScores scores{};
  Action chosen = Action::None;
  // Morale (M5): the unit's morale value, state and routed flag on the
  // captured tick (0 / Steady / false with morale off).
  double morale = 0.0;
  std::uint8_t morale_state = 1;  // MoraleState; 1 == Steady
  bool routed = false;
  // Squads (M6): the member's effective order, its obedience weight and the
  // per-action order score bonus on the captured tick (order None and zero
  // obedience for squadless units or with squads off).
  std::uint8_t order = 4;  // Order; 4 == None
  double obedience = 0.0;
  std::array<double, kActionCount> order_bonus{};
  // M10: consecutive pursuit ticks and whether the captured decision had
  // a fresh contact (Search), an ally in need (Help) or flank rights.
  std::uint32_t pursuit_ticks = 0;
  bool has_fresh_contact = false;
  bool ally_in_need = false;
  bool member_may_flank = false;
};

// Fixed-timestep headless battle simulation.
//
// Construction receives the configuration (and, for tests and future replay
// loading, an explicit initial state). tick() advances the world by exactly
// one tick through a fixed phase pipeline; run() ticks until termination.
// The class performs no file or terminal I/O and never reads the wall clock.
//
// Fair update order: each tick first decides every unit's target from a
// read-only snapshot, computes every move from that same snapshot, and only
// then resolves all attacks simultaneously. No unit gains an advantage from
// its position in the unit list.
//
// AI modes (M4): Legacy runs the M1-M3 AI (every unit always knows the
// nearest enemy and attacks whenever it is in range). Utility replaces it
// with sight-limited perception (grid radius queries), personality-weighted
// action scoring and per-action movement/attack rules; see docs/ai.md.
//
// Morale and routing (M5, utility mode only, docs/morale.md): morale moves
// with documented loss/gain sources; a Broken unit routs — it flees at full
// speed and cannot fight — and recovers only after a safe-time and
// morale-threshold requirement (hysteresis). A faction with no active units
// for rout_timeout_ticks ends the battle by FACTION_ROUTED. Morale-off
// battles are bit-identical to their M4 runs; legacy mode ignores morale.
//
// Squads, formations and commanders (M6, utility mode only, docs/squads.md):
// factions with a squad plan field squads of one commander plus `size`
// soldiers; assignment is deterministic (spatially contiguous groups, spawn
// order as tie-break). Idle or advancing soldiers steer to their formation
// slot (weighted separation/cohesion/alignment/slot seeking); a soldier
// drops formation whenever its AI picks Attack, Approach, Retreat or Flee.
// Commanders pick orders (advance/hold/regroup/retreat) from squad state;
// orders reach members with a distance-based delay inside the leadership
// radius and decay outside it. The order is a bounded score term scaled by
// an obedience weight (discipline, distance, morale state, cohesion) —
// never an override. A commander's death shocks nearby members and starts
// succession after a fixed delay (lowest-id alive, non-routed member);
// routed commanders lose command. Squads-off battles are bit-identical to
// their M5 runs; legacy mode ignores squads.
class Simulation {
 public:
  // Spawns both armies deterministically from config.seed.
  explicit Simulation(SimulationConfig config);

  // Takes an explicit initial state (used by tests). Battlefield dimensions
  // and limits still come from config. Unit ids must be non-zero, unique, and
  // alive units must have positive health. `explicit_personalities`, when
  // non-empty, overrides the hash-derived personalities unit by unit (same
  // size as initial_units; archetype indices refer to the personality table
  // in use, or kCustomArchetype).
  Simulation(SimulationConfig config, std::vector<Unit> initial_units,
             std::vector<Personality> explicit_personalities = {});

  // Advances the battle by one tick; no-op once the battle has terminated.
  void tick();

  // Runs ticks until the battle terminates; returns the final result.
  [[nodiscard]] BattleResult run();

  [[nodiscard]] bool finished() const noexcept { return finished_; }
  [[nodiscard]] BattleResult result() const;
  [[nodiscard]] const World& world() const noexcept { return world_; }
  [[nodiscard]] const SimulationConfig& config() const noexcept {
    return config_;
  }
  [[nodiscard]] std::uint64_t tick_index() const noexcept {
    return world_.tick_count;
  }

  // What one unit sensed on the last tick it was alive for (utility mode);
  // the zero state (no enemy, no counts) before the first tick, for units
  // that never decided, and in legacy mode.
  [[nodiscard]] const Perception& perception(
      std::size_t unit_index) const noexcept {
    return unit_index < last_perception_.size() ? last_perception_[unit_index]
                                                : kNoPerception;
  }

  // The personality table this battle uses (config pointer or built-in).
  [[nodiscard]] const PersonalityTable& personality_table() const noexcept {
    return *effective_personalities_;
  }

  // True when morale and routing are active (M5: utility mode + morale on).
  [[nodiscard]] bool morale_enabled() const noexcept {
    return morale_enabled_;
  }

  // True when squads are active (M6: utility mode + squads enabled + at
  // least one faction squad plan).
  [[nodiscard]] bool squads_active() const noexcept {
    return squads_active_;
  }

  // True when M10 tactics are active (utility mode + tactics enabled):
  // combat variance, decision intervals, memory and the Search/Help/Flank
  // actions. With tactics off, battles and exports are byte-identical to
  // their M1-M9 runs.
  [[nodiscard]] bool tactics_active() const noexcept {
    return tactics_active_;
  }

  // True when contact seeking is active (utility + tactics + contact
  // seek enabled): parked units retarget Advance to the faction's last
  // known enemy estimate. Off reproduces Part 1 byte-for-byte.
  [[nodiscard]] bool contact_seek_active() const noexcept {
    return contact_seek_active_;
  }

  // Replay intervention (M10 Part 2b): kill one unit by id before the
  // next tick's phases run (health zeroed; apply_deaths processes it,
  // and a Death event with killer 0 is recorded if events are on).
  // Returns false when the id is unknown or already dead.
  [[nodiscard]] bool intervene_kill(std::uint32_t unit_id);

  // Player orders (m11b). Issues one order to each listed squad of
  // `faction` (a wrong-faction or unknown squad id rejects the whole
  // call; nothing is half-applied). The order travels through the
  // commander system: delivery lands at
  //   issue_tick + ceil(order_delay_per_unit *
  //                     distance(faction HQ, acting commander or
  //                              squad centroid when leaderless))
  // plus succession_delay_ticks when command has been lost (the order
  // goes through the successor) or player_orders.leaderless_delay_ticks
  // when no member is eligible to lead (then delivered at reduced
  // obedience). The HQ is the faction's spawn band center, fixed at
  // battle start. Obedience is the M6 weight (discipline, distance to
  // the commander inside the leadership radius, morale state, squad
  // cohesion) computed at delivery; routed units never obey (their
  // action is the forced Flee). Orders are bounded score terms — they
  // nudge action scores, never override availability. Per faction per
  // tick at most commanders.player_orders.max_per_tick calls succeed;
  // excess calls are rejected and counted. Inert when config
  // orders_enabled is false (all calls rejected). With no order ever
  // issued, battles are byte-identical with the machinery on or off.
  // MoveTo/AttackMove/Retreat snap the point to the nearest passable
  // cell at issue time (a point that cannot be snapped rejects the
  // call); ChangeFormation takes a formation id from formations.json.
  [[nodiscard]] bool issue_order(
      Faction faction, const std::vector<std::uint32_t>& squad_ids,
      PlayerOrderType type, double x = 0.0, double y = 0.0,
      const std::string& formation = "");

  // Per-faction player-order counters for the report/export (m11b).
  struct PlayerOrderReport {
    std::uint64_t issued = 0;
    std::uint64_t delivered = 0;    // applied to a living squad
    std::uint64_t rejected = 0;     // invalid / rate-limited / disabled
    std::uint64_t dropped = 0;      // squad collapsed before delivery
    std::uint64_t delay_ticks_sum = 0;   // over delivered orders
    double obedience_sum = 0.0;          // over delivered orders
  };
  [[nodiscard]] const PlayerOrderReport& player_order_report(
      Faction faction) const noexcept {
    return player_order_reports_[static_cast<std::size_t>(faction)];
  }

  // Canonical, byte-stable state hash (M10 Part 2b): 32 hex chars of
  // FNV-1a 128 over every piece of state that can influence the battle's
  // future. A recorded run and a re-run diverge iff the hashes diverge.
  [[nodiscard]] std::string state_hash() const;

  // Timeline samples (M10 Part 2a): one row every timeline_interval
  // ticks plus a final row; empty when sampling is off.
  [[nodiscard]] const std::vector<TimelineRow>& timeline() const noexcept {
    return timeline_rows_;
  }


  // The faction-level enemy estimate (mean position of the nearest
  // enemies members last saw) and the 1-based tick it was last updated
  // on (0 = the faction has never seen an enemy). Diagnostic/testing.
  [[nodiscard]] std::uint64_t faction_estimate_tick(
      Faction faction) const noexcept {
    return estimate_tick_[static_cast<std::size_t>(faction)];
  }
  [[nodiscard]] double faction_estimate_x(Faction faction) const noexcept {
    return estimate_x_[static_cast<std::size_t>(faction)];
  }
  [[nodiscard]] double faction_estimate_y(Faction faction) const noexcept {
    return estimate_y_[static_cast<std::size_t>(faction)];
  }

  // The tactics configuration in use (never null once constructed).
  [[nodiscard]] const TacticsConfig& tactics_config() const noexcept {
    return *effective_tactics_;
  }

  // All squads in global id order (Red squads, then Blue; empty with squads
  // off). Exposed for the JSON export, the CLI squad debug print and tests.
  [[nodiscard]] const std::vector<Squad>& squads() const noexcept {
    return squads_;
  }

  // The terrain grid of this battle, or nullptr on the classic open field
  // (M8). Borrowed from the config; valid for the Simulation's lifetime.
  [[nodiscard]] const TerrainGrid* terrain() const noexcept {
    return terrain_;
  }

  // The squad a unit belongs to (global squad id), or -1 when squadless.
  [[nodiscard]] std::int32_t squad_of(std::size_t unit_index) const noexcept {
    return unit_index < unit_squad_.size() ? unit_squad_[unit_index] : -1;
  }

  // The unit's full squad view (M6); squad_id -1 (and defaults elsewhere)
  // when the unit is squadless or squads are off.
  [[nodiscard]] UnitSquadInfo unit_squad_info(
      std::size_t unit_index) const noexcept {
    UnitSquadInfo info;
    if (!squads_active_ || unit_index >= unit_squad_.size() ||
        unit_squad_[unit_index] < 0) {
      return info;
    }
    const Squad& squad =
        squads_[static_cast<std::size_t>(unit_squad_[unit_index])];
    info.squad_id = unit_squad_[unit_index];
    info.order = squad.members[unit_member_[unit_index]].order;
    info.formation = squad.formation;
    info.cohesion = squad.cohesion;
    return info;
  }

  // The world position of one squad member's formation slot on this tick —
  // exactly what steering seeks (commander anchor while commanded, else the
  // squad centroid; slot offset rotated by the faction's formation axes).
  // For the M7 viewer. Returns false when squads are off or the squad or
  // member index does not exist.
  [[nodiscard]] bool squad_slot_position(std::int32_t squad_id,
                                         std::size_t member_index, double& x,
                                         double& y) const noexcept;

  // The formation definition id for a formation kind ("line", "column",
  // "loose").
  [[nodiscard]] static std::string formation_name(
      FormationKind kind) noexcept;

  // Arms the AI debug capture (see AiDebugCapture). Marks unit_found by
  // scanning the world for the requested id.
  void set_ai_debug(AiDebugCapture capture) noexcept {
    ai_debug_ = capture;
    ai_debug_.unit_found = false;
    if (capture.unit_id != 0) {
      for (const Unit& unit : world_.units) {
        if (unit.id == capture.unit_id) {
          ai_debug_.unit_found = true;
          break;
        }
      }
    }
  }
  [[nodiscard]] const AiDebugCapture& ai_debug() const noexcept {
    return ai_debug_;
  }

  // Intervention hook (M6 acceptance experiments): zeroes a unit's health
  // between ticks. The death resolves in the next tick's apply_deaths with
  // full semantics — morale events and, for an acting commander, the
  // command loss, member shock and succession schedule. Throws
  // std::invalid_argument for an unknown index or an already dead unit.
  // Never used by the battle itself; determinism is unaffected.
  void slay_unit(std::size_t unit_index) {
    if (unit_index >= world_.units.size()) {
      throw std::invalid_argument("slay_unit: no unit at index " +
                                  std::to_string(unit_index));
    }
    if (!world_.units[unit_index].alive) {
      throw std::invalid_argument("slay_unit: unit " +
                                  std::to_string(world_.units[unit_index].id) +
                                  " is already dead");
    }
    world_.units[unit_index].health = 0.0;
  }

 private:
  // Per-tick scratch buffers, sized once and reused (no allocations inside
  // the simulation loop).
  struct PendingMove {
    double x = 0.0;
    double y = 0.0;
    bool moves = false;
    bool goal_seeking = false;  // M8: Approach/Advance/formation steering
  };
  struct PendingAttack {
    std::size_t attacker = 0;
    std::size_t target = 0;
    double damage = 0.0;
  };

  void spawn_armies();

  // --- Terrain (M8) ------------------------------------------------------
  // Stuck detection constants: a goal-seeking unit that actually moves
  // less than kStuckEpsilon world units for kStuckTickThreshold
  // consecutive ticks follows its faction's flow field for
  // kFlowFollowTicks ticks, then resumes normal steering.
  static constexpr double kStuckEpsilon = 0.05;
  static constexpr std::uint16_t kStuckTickThreshold = 20;
  static constexpr std::uint16_t kFlowFollowTicks = 60;

  void init_terrain();  // validation + first flow fields (constructor)
  void refresh_flow_fields();  // recompute when an objective cell changed
  // The observer's sight radius with its cell's sight modifier applied
  // (capped by config.terrain_sight_max_scale); the plain radius without
  // terrain.
  [[nodiscard]] double effective_sight(const Unit& unit) const noexcept;
  // The flow-field unit step for a unit of `faction` standing at (x, y);
  // false when the cell has no direction.
  [[nodiscard]] bool flow_step(std::size_t faction, double x, double y,
                               double& dx, double& dy) const noexcept;
  // The world position of one member's formation slot: commander anchor
  // (or centroid when leaderless) plus the rotated slot offset, nudged
  // off impassable cells (deterministic ring scan) when terrain exists.
  [[nodiscard]] bool formation_slot_world(const Squad& squad,
                                          std::size_t member_index, double& x,
                                          double& y) const noexcept;
  // Terrain commit for one pending move: scales the step by the entered
  // cell's speed multiplier, wall-slides around impassable cells and
  // records stuck progress. Returns the actual distance moved.
  double commit_move_terrain(std::size_t index, const PendingMove& move);
  // Moves (x, y) to the nearest passable cell center by a deterministic
  // expanding-ring scan (up to 16 cells). False when none is found.
  [[nodiscard]] bool snap_to_passable(double& x, double& y) const noexcept;

  const TerrainGrid* terrain_ = nullptr;
  std::array<FlowField, kFactionCount> flow_{};
  std::array<std::uint64_t, kFactionCount> flow_target_cell_{};  // packed, max = none
  std::vector<std::uint16_t> stuck_ticks_;        // consecutive stuck ticks
  std::vector<std::uint16_t> max_stuck_stretch_;  // per-unit maximum
  std::vector<bool> counted_stuck_;               // counted in units_stuck_over_100_
  std::vector<std::uint64_t> flow_follow_until_;  // tick until which to follow
  std::vector<std::uint64_t> terrain_kills_;      // by terrain type index
  std::uint32_t units_stuck_over_100_ = 0;
  // m11a: navigation v2 — a formation member whose projected slot is
  // farther than kSlotMarchDist world units away marches along the flow
  // field instead of beelining to the slot (corridors: the whole squad
  // columns through gates; open ground: boids steering re-forms the
  // shape once the slot is near). kSlotMarchDist = 2.4 cells at the
  // earth-map cell size, comfortably above the slot spacing (3.0) so a
  // member ON its slot still steers by the shape rules.
  static constexpr double kSlotMarchDist = 6.0;

  // m12 audit: arrival radius for point orders (MoveTo / AttackMove /
  // Retreat-to-point). Within it a unit holds at the ordered point instead
  // of resuming the march to the faction objective (on terrain maps the
  // squad flow field has no direction on its target cell, which used to
  // fall through to a straight-line faction-objective beeline: the squad
  // oscillated around the point forever).
  static constexpr double kOrderArrivalDist = 3.0;
  // m11a: first tick any unit saw an enemy (0 = none yet).
  std::uint64_t first_contact_tick_ = 0;
  void assign_personalities(
      const std::vector<Personality>& explicit_personalities);
  // Morale initialization (M5). Spawned units start at
  // base + bravery_scale * bravery; explicit initial states keep the morale
  // and routed values they were constructed with (only the state is
  // re-derived from the value).
  void initialize_morale(bool explicit_state);
  // Squad assignment (M6): deterministic at spawn — per faction, soldiers
  // and commanders are sorted by (y, x, id) so squads are spatially
  // contiguous groups, then sliced into `count` squads of `size` soldiers
  // plus one commander each. Also computes each faction's formation axes
  // (forward toward the objective, lateral perpendicular) and the squads'
  // initial centroid/cohesion so tick-1 steering sees sane values.
  void assign_squads();
  // Recomputes one squad's alive-member centroid and cohesion metric.
  void update_squad_metrics(Squad& squad);
  // Utility AI (M4): perception + scoring for one unit, from the tick-start
  // snapshot. Fills perception and scores and returns the chosen action
  // (Action::Flee for routed units, whose scores stay -1).
  Action decide_unit_action(std::size_t index, Perception& perception,
                            ActionScores& scores);
  // Phase 0 of the project pipeline: keep the spatial index current. The
  // grid is built once after spawning (or adopting an explicit state) and
  // then maintained incrementally in fixed unit-index order: moved units are
  // re-bucketed after movement, dead units are removed on death (inside
  // apply_deaths).
  void update_grid_after_movement();
  // Formation steering (M6) for one squad member whose action is Hold or
  // Advance: the weighted separation/cohesion/alignment/slot-seeking blend.
  // Returns true when it computed a move (the caller skips the normal
  // Hold/Advance handling).
  bool steer_in_formation(std::size_t index, const Unit& unit,
                          const Squad& squad, const SquadMemberState& member);

  // Fixed pipeline phases (see docs/architecture.md for the full mapping).
  void resolve_cooldowns();  // 0. timers
  void decide_targets();     // 1. perception + decision (read-only)
  void resolve_movement();   // 2. movement: snapshot, then commit
  void resolve_combat();     // 3. combat: decide, then resolve simultaneously
  void apply_deaths();       // 4. deaths
  void update_morale();      // 5. morale, routing and recovery (M5)
  void update_squads();      // 6. squads: metrics, command, orders (M6)
  void update_termination();  // 7. statistics, termination, commit tick

  SimulationConfig config_;
  World world_;
  Rng rng_;
  std::array<FactionReport, kFactionCount> faction_reports_{};
  // Per-type counters, one vector per faction sized like the unit type table
  // (empty for legacy battles without a table). Filled during the battle and
  // copied into BattleResult::FactionReport::by_type by result().
  std::array<std::vector<TypeReport>, kFactionCount> type_reports_{};
  bool use_type_reports_ = false;
  std::vector<int> target_index_;             // per unit, this tick's target
  std::vector<PendingMove> pending_moves_;    // per unit, this tick's move
  std::vector<PendingAttack> pending_attacks_;  // this tick's attack list
  SpatialHashGrid grid_;  // maintained in grid neighbor mode or utility AI
  bool use_grid_ = false;
  // Utility AI state (M4).
  const PersonalityTable* effective_personalities_ = nullptr;
  std::array<double, kFactionCount> objective_x_{};  // faction -> objective
  std::array<double, kFactionCount> objective_y_{};
  std::vector<Perception> last_perception_{};   // per unit, last alive tick
  std::vector<std::uint32_t> perceive_buffer_;  // reused radius-query buffer
  std::vector<std::uint8_t> action_commit_{};   // commit ticks left per unit
  std::array<std::array<std::uint64_t, kActionCount>, kFactionCount>
      action_counts_{};  // cumulative per-faction action totals
  // Morale and routing state (M5).
  bool morale_enabled_ = false;
  const MoraleConfig* effective_morale_ = nullptr;
  double max_sight_ = 0.0;  // largest sight radius in this battle
  std::vector<double> damage_this_tick_{};      // per unit, combat phase
  std::vector<std::uint32_t> kills_this_tick_{};  // per unit, combat phase
  std::vector<std::uint64_t> safe_ticks_{};      // per unit, consecutive safe
  std::vector<std::uint32_t> ally_death_count_{};  // per unit, this tick
  struct DeathEvent {           // one ally death, for nearby-loss morale
    double x = 0.0;
    double y = 0.0;
    Faction faction = Faction::Red;
  };
  std::vector<DeathEvent> death_events_{};
  std::vector<DeathEvent> rout_events_{};  // breaks from the previous tick
  std::vector<std::uint32_t> death_observer_buffer_{};  // reused query buffer
  std::vector<std::uint32_t> ally_rout_count_{};  // per unit, this tick
  std::array<RoutingStats, kFactionCount> routing_stats_{};
  std::array<std::uint64_t, kFactionCount> inactive_ticks_{};  // consecutive
  // Squads, formations and commanders (M6).
  bool squads_active_ = false;
  const FormationConfig* formations_ = nullptr;
  const CommanderConfig* commanders_ = nullptr;
  std::vector<Squad> squads_{};               // global id order
  std::vector<std::int32_t> unit_squad_{};    // per unit, squad id or -1
  std::vector<std::uint32_t> unit_member_{};  // per unit, index into squad members
  std::array<SquadStats, kFactionCount> squad_stats_{};
  std::array<double, kFactionCount> forward_x_{};  // formation axes per faction
  std::array<double, kFactionCount> forward_y_{};
  std::array<double, kFactionCount> lateral_x_{};
  std::array<double, kFactionCount> lateral_y_{};
  std::vector<std::uint32_t> separation_buffer_{};  // reused steering query
  std::vector<std::uint32_t> squad_perceive_buffer_{};  // commander order query
  // Player orders (m11b): the in-flight orders (ascending deliver_tick,
  // then issue order), the per-squad flow fields of point orders
  // (MoveTo / AttackMove / Retreat to a point; computed once at
  // delivery, cached until the order ends), the per-faction HQ (spawn
  // band center) the courier distance is measured from, this tick's
  // issue count for the rate limit, and the report counters.
  std::vector<PlayerOrder> pending_orders_{};
  std::vector<FlowField> squad_flow_{};
  std::vector<std::uint64_t> squad_flow_target_{};  // packed cell, max = none
  std::array<double, kFactionCount> hq_x_{};
  std::array<double, kFactionCount> hq_y_{};
  std::array<std::uint32_t, kFactionCount> orders_issued_this_tick_{};
  std::array<PlayerOrderReport, kFactionCount> player_order_reports_{};
  // m11b: deliver due orders (called from update_squads after succession,
  // before the commander AI re-evaluates), and the delivery itself.
  void process_pending_orders(std::uint64_t tick_number);
  void apply_player_order(Squad& squad, const PlayerOrder& order,
                          std::uint64_t tick_number);
  // Ends a squad's player order (command loss, collapse, replacement);
  // clears the point state and the squad flow field.
  void clear_player_order(Squad& squad);
  // The flow-field step for one unit: the squad's own field while a
  // point order is active on its squad, else the faction field.
  [[nodiscard]] bool order_flow_step(std::size_t unit_index,
                                     std::size_t faction, double x, double y,
                                     double& dx, double& dy) const noexcept;
  // Tactical depth (M10).
  bool tactics_active_ = false;
  const TacticsConfig* effective_tactics_ = nullptr;
  // Per unit: the goal point of this tick's Search/Help/Flank action
  // (set in decide, consumed by movement; cleared every tick).
  std::vector<double> tactic_goal_x_{};
  std::vector<double> tactic_goal_y_{};
  std::vector<std::uint8_t> tactic_goal_set_{};
  // Per squad: some member saw an enemy on the previous tick (computed
  // before the decide loop from last_perception_).
  std::vector<std::uint8_t> squad_engaged_{};
  // Contact seeking (M10 continuation): the faction-level enemy estimate
  // (per faction: position + 1-based tick of the last update) and the per
  // unit count of consecutive ticks without a visible enemy. Once the
  // streak reaches contact_seek.idle_ticks, the unit's freshest contact
  // becomes the estimate (while it is fresh enough), so Search heads for
  // it instead of a stale personal memory.
  bool contact_seek_active_ = false;
  std::array<double, kFactionCount> estimate_x_{};
  std::array<double, kFactionCount> estimate_y_{};
  std::array<std::uint64_t, kFactionCount> estimate_tick_{};
  std::vector<std::uint32_t> contactless_streak_{};
  // Per unit: contact seek engaged on the previous decision (for the
  // seek_start event's false -> true transition).
  std::vector<std::uint8_t> was_seeking_{};

  // m13 item 2 (--ai-avail v2): per-ally Help target counts this tick
  // (reset at the top of the decide phase; decisions run in unit index
  // order so the cap is deterministic) and a scratch buffer for the
  // routed-ally-near-combat probe.
  std::vector<std::uint32_t> help_target_count_{};
  std::vector<std::uint32_t> help_probe_buffer_{};

  // m13 item 2 (--end v2): consecutive ticks each faction's active count
  // stayed below the routed threshold, and the initial alive counts the
  // threshold scales from.
  std::array<std::uint64_t, kFactionCount> below_threshold_ticks_{};
  std::array<std::uint32_t, kFactionCount> initial_alive_{};

  // m13 item 2 (--end v2, stalemate): a ring of the combined
  // attacker count (alive units, both factions, choosing Attack) per
  // tick, holding the last termination.stalemate_ticks ticks. Filled
  // from first contact; judged only once full, so the window is always
  // real data (a battle that fought 500 ticks and then went quiet is
  // judged on the quiet part alone).
  std::vector<std::uint32_t> attack_count_ring_{};
  std::size_t stalemate_ring_pos_ = 0;
  std::size_t stalemate_samples_ = 0;
  // Battle events (M10 Part 2a): owned by the caller (config_.events).
  EventRecorder* events_ = nullptr;
  // Timeline sampling (M10 Part 2a).
  std::vector<TimelineRow> timeline_rows_{};
  // M10 Part 2a helper: sample one timeline row for this tick.
  void sample_timeline();
  // M10 helper: update squad contact lists from this tick's perceptions.
  void update_squad_contacts();
  // M10 continuation helper: refresh the faction-level enemy estimates
  // from this tick's perceptions.
  void update_faction_estimates();
  AiDebugCapture ai_debug_{};
  static inline const Perception kNoPerception{};
  bool finished_ = false;
  TerminationReason reason_ = TerminationReason::None;
  bool has_winner_ = false;
  Faction winner_ = Faction::Red;
};

}  // namespace sim
