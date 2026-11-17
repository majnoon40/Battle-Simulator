#include "sim/simulation.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "sim/ai.hpp"
#include "sim/combat.hpp"
#include "sim/morale.hpp"
#include "sim/neighbors.hpp"
#include "sim/personalities.hpp"
#include "sim/spatial_hash.hpp"

namespace sim {
namespace {

std::size_t faction_index(Faction faction) noexcept {
  return static_cast<std::size_t>(faction);
}

}  // namespace

Simulation::Simulation(SimulationConfig config)
    : Simulation(std::move(config), {}, {}) {}

Simulation::Simulation(SimulationConfig config,
                       std::vector<Unit> initial_units,
                       std::vector<Personality> explicit_personalities)
    : config_(std::move(config)), rng_(config_.seed) {
  validate_config(config_);
  // Remember whether this is an explicit initial state before the vector is
  // moved from below: explicit states keep their preset morale and routed
  // values (tests construct broken or recovering units that way).
  const bool explicit_state = !initial_units.empty();

  world_.width = config_.battlefield_width;
  world_.height = config_.battlefield_height;
  terrain_ = config_.terrain;

  // Per-type reports exist exactly when a unit type table drives this battle.
  use_type_reports_ = config_.unit_types != nullptr;
  if (use_type_reports_) {
    for (Faction faction : kAllFactions) {
      type_reports_[faction_index(faction)].resize(config_.unit_types->size());
      for (std::size_t t = 0; t < config_.unit_types->size(); ++t) {
        type_reports_[faction_index(faction)][t].type_id =
            config_.unit_types->types[t].id;
      }
    }
  }

  if (initial_units.empty()) {
    spawn_armies();
  } else {
    // Explicit initial state: validate identifiers before adopting it.
    std::unordered_set<std::uint32_t> seen_ids;
    for (const Unit& unit : initial_units) {
      if (unit.id == 0) {
        throw std::invalid_argument("unit id 0 is reserved; ids must be >= 1");
      }
      if (!seen_ids.insert(unit.id).second) {
        throw std::invalid_argument("duplicate unit id " +
                                    std::to_string(unit.id));
      }
      if (unit.alive && !(unit.health > 0.0)) {
        throw std::invalid_argument("alive unit " + std::to_string(unit.id) +
                                    " must have health > 0");
      }
      if (use_type_reports_ &&
          unit.type_index >= config_.unit_types->size()) {
        throw std::invalid_argument(
            "unit " + std::to_string(unit.id) + " has type index " +
            std::to_string(unit.type_index) +
            " outside the unit type table (size " +
            std::to_string(config_.unit_types->size()) + ")");
      }
    }
    world_.units = std::move(initial_units);
  }

  for (const Unit& unit : world_.units) {
    const std::size_t index = faction_index(unit.faction);
    faction_reports_[index].initial += 1;
    if (use_type_reports_) {
      type_reports_[index][unit.type_index].spawned += 1;
    }
  }

  // Personalities (M4): hash-derived from (seed, unit id) in every mode —
  // never from the spawn RNG stream — or taken from the explicit override
  // vector (tests). Pure extra state: legacy behavior is untouched.
  effective_personalities_ = config_.personalities != nullptr
                                 ? config_.personalities
                                 : &default_personality_table();
  assign_personalities(explicit_personalities);

  // Morale and routing (M5): utility mode only, so legacy battles and
  // morale-off utility battles stay bit-identical to their M1-M4 runs.
  initialize_morale(explicit_state);

  // Objective points (M4): each faction advances toward the center of the
  // opposing spawn band unless the configuration overrides the point.
  const double band = world_.width * config_.spawn_band_fraction;
  for (Faction faction : kAllFactions) {
    const std::size_t index = faction_index(faction);
    if (config_.objectives[index].explicit_set) {
      objective_x_[index] = config_.objectives[index].x;
      objective_y_[index] = config_.objectives[index].y;
    } else if ((faction == Faction::Red) != config_.swap_sides) {
      // Default objective: the center of the opposing spawn band. With
      // --swap-sides (M10) Red holds the eastern band, so the sides flip.
      objective_x_[index] = world_.width - band / 2.0;
      objective_y_[index] = world_.height / 2.0;
    } else {
      objective_x_[index] = band / 2.0;
      objective_y_[index] = world_.height / 2.0;
    }
  }

  // Squads (M6): assignment happens once, after the armies exist and the
  // objectives are known (the formation axes point at them). Pure extra
  // state — with no squad plans, or squads disabled, nothing below changes
  // and battles stay bit-identical to their M5 runs.
  assign_squads();

  // Player orders (m11b): the HQ each faction issues from is its spawn
  // band center (the band it actually deploys in — with swap_sides Red
  // deploys in the east, so its HQ is the eastern band center); the
  // per-squad flow fields stay empty until a point order is delivered.
  for (Faction faction : kAllFactions) {
    const std::size_t index = faction_index(faction);
    const bool west = (faction == Faction::Red) != config_.swap_sides;
    const double band = world_.width * config_.spawn_band_fraction;
    hq_x_[index] = west ? band / 2.0 : world_.width - band / 2.0;
    hq_y_[index] = world_.height / 2.0;
  }
  squad_flow_.assign(squads_.size(), FlowField{});
  squad_flow_target_.assign(squads_.size(),
                            std::numeric_limits<std::uint64_t>::max());
  orders_issued_this_tick_ = {};
  player_order_reports_ = {};

  // Tactical depth (M10): utility mode only. With tactics off there is no
  // battle-time randomness, the M4-M9 action set is scored and the commit
  // time is the M4 constant, so every M1-M9 reference reproduces
  // byte-for-byte. The decision interval is drawn from the counter RNG
  // (channel 3) — never from the spawn stream.
  tactics_active_ = config_.ai_mode == AiMode::Utility && config_.tactics_enabled;
  effective_tactics_ = config_.tactics != nullptr
                           ? config_.tactics
                           : &default_tactics_config();
  tactic_goal_x_.assign(world_.units.size(), 0.0);
  tactic_goal_y_.assign(world_.units.size(), 0.0);
  tactic_goal_set_.assign(world_.units.size(), 0);
  squad_engaged_.assign(squads_.size(), 0);
  contact_seek_active_ = tactics_active_ && config_.contact_seek_enabled;
  contactless_streak_.assign(world_.units.size(), 0);
  was_seeking_.assign(world_.units.size(), 0);
  // m13 item 2: Help caps, the routed-threshold streaks, the initial
  // strengths the threshold scales from, and the stalemate ring.
  help_target_count_.assign(world_.units.size(), 0);
  help_probe_buffer_.clear();
  below_threshold_ticks_.fill(0);
  initial_alive_.fill(0);
  for (const Unit& unit : world_.units) {
    if (unit.alive) {
      ++initial_alive_[faction_index(unit.faction)];
    }
  }
  attack_count_ring_.assign(
      static_cast<std::size_t>(std::min<std::uint64_t>(
          effective_morale_->termination.stalemate_ticks, 10000)),
      0);
  stalemate_ring_pos_ = 0;
  stalemate_samples_ = 0;
  // m13 item 3: combat depth. Utility mode only (legacy reproduces M1
  // exactly); every buffer stays zero under v1 so hashes and exports
  // are unchanged.
  static const CombatConfig kDefaultCombat;
  effective_combat_ =
      config_.combat != nullptr ? config_.combat : &kDefaultCombat;
  combat_v2_ = config_.combat_v2 && config_.ai_mode == AiMode::Utility;
  aura_active_.assign(world_.units.size(), 0);
  defensive_stance_.assign(world_.units.size(), 0);
  rear_hits_this_tick_.assign(world_.units.size(), 0);
  attacked_this_tick_.assign(world_.units.size(), 0);
  charge_window_ = effective_combat_->charge.window_ticks;
  charge_ring_.assign(2 * charge_window_ * world_.units.size(), 0.0);
  charge_ring_pos_.assign(world_.units.size(), 0);
  combat_probe_buffer_.clear();
  if (combat_v2_) {
    // Every unit starts facing its faction's objective (across the
    // field, toward the enemy).
    for (Unit& unit : world_.units) {
      const std::size_t faction = faction_index(unit.faction);
      unit.facing = std::atan2(objective_y_[faction] - unit.y,
                               objective_x_[faction] - unit.x);
    }
  }
  events_ = config_.events;
  for (Unit& unit : world_.units) {
    unit.decision_interval =
        static_cast<std::uint8_t>(decision_interval_of(config_.seed, unit.id));
  }

  // Scratch buffers are sized once: at most one attack per alive unit per
  // tick, so reserving per-unit capacity keeps the tick loop allocation-free
  // in steady state.
  pending_attacks_.reserve(world_.units.size());
  last_perception_.assign(world_.units.size(), Perception{});
  action_commit_.assign(world_.units.size(), 0);
  damage_this_tick_.assign(world_.units.size(), 0.0);
  kills_this_tick_.assign(world_.units.size(), 0);
  safe_ticks_.assign(world_.units.size(), 0);
  ally_death_count_.assign(world_.units.size(), 0);
  ally_rout_count_.assign(world_.units.size(), 0);

  // Terrain (M8): validate the grid against the battlefield, spawn bands
  // and objectives, size the stuck tracking, and compute the flow fields.
  // With no map nothing below runs and battles stay bit-identical.
  if (terrain_ != nullptr) {
    init_terrain();
  }

  // Spatial index (M3/M4): used by grid-mode legacy targeting and always by
  // utility-mode perception (radius queries). Built in fixed unit-index
  // order; it is then maintained incrementally (movement updates, death
  // removals). Bucket capacity is reserved for ~64x the spawn-band density
  // so buckets rarely grow during front convergence (the measured residual
  // allocation counts are printed by tools/analysis/profile_phases.cpp and
  // asserted by the steady-state test).
  use_grid_ = config_.neighbor_mode == NeighborMode::Grid ||
              config_.ai_mode == AiMode::Utility;
  if (use_grid_) {
    const double band_cells =
        std::ceil(world_.width * config_.spawn_band_fraction /
                  config_.grid_cell_size) *
        std::ceil(world_.height / config_.grid_cell_size);
    const std::size_t bucket_reserve = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::ceil(
            64.0 * static_cast<double>((world_.units.size() + 1) / 2) /
            band_cells)),
        4, 16384);
    grid_.configure(world_.width, world_.height, config_.grid_cell_size,
                    bucket_reserve);
    grid_.build(world_.units);
  }
}

// Personalities never touch rng_: the same (seed, unit id, table) triple
// yields the same personality on every platform, in every AI mode.
void Simulation::assign_personalities(
    const std::vector<Personality>& explicit_personalities) {
  if (!explicit_personalities.empty()) {
    if (explicit_personalities.size() != world_.units.size()) {
      throw std::invalid_argument(
          "explicit personalities must match the unit count (" +
          std::to_string(explicit_personalities.size()) + " given, " +
          std::to_string(world_.units.size()) + " units)");
    }
    for (std::size_t i = 0; i < world_.units.size(); ++i) {
      const std::uint8_t archetype = explicit_personalities[i].archetype;
      if (archetype != kCustomArchetype &&
          static_cast<std::size_t>(archetype) >=
              effective_personalities_->size()) {
        throw std::invalid_argument(
            "personality archetype index " + std::to_string(archetype) +
            " is outside the personality table (size " +
            std::to_string(effective_personalities_->size()) + ")");
      }
      world_.units[i].personality = explicit_personalities[i];
    }
    return;
  }
  for (Unit& unit : world_.units) {
    unit.personality =
        personality_for(config_.seed, unit.id, *effective_personalities_);
  }
}

// Morale initialization (M5). With morale disabled (legacy mode, or the
// utility mode with morale off) nothing is touched: unit morale stays 0,
// the state stays Steady and no routed flag is ever set, so behavior is
// bit-identical to the M4 runs.
void Simulation::initialize_morale(bool explicit_state) {
  effective_morale_ =
      config_.morale != nullptr ? config_.morale : &default_morale_config();
  morale_enabled_ =
      config_.ai_mode == AiMode::Utility && config_.morale_enabled;
  if (!morale_enabled_) {
    // Disabled: scrub any preset routing so behavior is bit-identical to the
    // M4 runs regardless of how the initial state was constructed.
    for (Unit& unit : world_.units) {
      unit.routed = false;
    }
    return;
  }
  const MoraleConfig& morale = *effective_morale_;
  for (Unit& unit : world_.units) {
    if (explicit_state) {
      // Explicit initial states carry their own morale and routed values
      // (tests construct broken or recovering units this way); only the
      // state is re-derived so it always matches the value.
      unit.morale = std::clamp(unit.morale, 0.0, 100.0);
    } else {
      unit.morale = starting_morale(unit.personality.bravery, morale.start);
      unit.routed = false;
    }
    unit.morale_state = static_cast<std::uint8_t>(
        state_from_morale(unit.morale, morale.states));
  }
  for (const Unit& unit : world_.units) {
    max_sight_ = std::max(max_sight_, unit.stats.sight_radius);
  }
}

std::string Simulation::formation_name(FormationKind kind) noexcept {
  switch (kind) {
    case FormationKind::Line:
      return "line";
    case FormationKind::Column:
      return "column";
    case FormationKind::Loose:
      return "loose";
  }
  return "line";
}

// Squad metrics: the alive-member centroid and the cohesion metric
// clamp(1 - mean distance to centroid / cohesion_reference, 0, 1). Called at
// assignment (so tick-1 steering sees sane values) and once per tick in
// update_squads; steering and obedience read the last computed values, the
// same one-tick-stale pattern as last_perception_.
void Simulation::update_squad_metrics(Squad& squad) {
  double sum_x = 0.0;
  double sum_y = 0.0;
  std::size_t alive = 0;
  for (const SquadMemberState& member : squad.members) {
    const Unit& unit = world_.units[member.unit_index];
    if (unit.alive) {
      sum_x += unit.x;
      sum_y += unit.y;
      ++alive;
    }
  }
  if (alive == 0) {
    squad.cohesion = 0.0;  // centroid keeps its last value; nobody reads it
    return;
  }
  squad.centroid_x = sum_x / static_cast<double>(alive);
  squad.centroid_y = sum_y / static_cast<double>(alive);
  double distance_sum = 0.0;
  for (const SquadMemberState& member : squad.members) {
    const Unit& unit = world_.units[member.unit_index];
    if (!unit.alive) {
      continue;
    }
    distance_sum += std::sqrt((unit.x - squad.centroid_x) *
                                  (unit.x - squad.centroid_x) +
                              (unit.y - squad.centroid_y) *
                                  (unit.y - squad.centroid_y));
  }
  squad.cohesion =
      std::clamp(1.0 - distance_sum / static_cast<double>(alive) /
                           formations_->steering.cohesion_reference,
                 0.0, 1.0);
}

void Simulation::assign_squads() {
  squads_active_ = config_.ai_mode == AiMode::Utility && config_.squads_enabled;
  bool any_plan = false;
  for (Faction faction : kAllFactions) {
    any_plan = any_plan ||
               config_.squad_plans[faction_index(faction)].has_value();
  }
  if (!squads_active_ || !any_plan) {
    squads_active_ = false;
    return;
  }
  formations_ = config_.formations != nullptr
                    ? config_.formations
                    : &default_formation_config();
  commanders_ = config_.commanders != nullptr
                    ? config_.commanders
                    : &default_commander_config();

  // The default formation definition (kind + spacing) every squad uses.
  const FormationConfig::Definition* default_definition = nullptr;
  for (const FormationConfig::Definition& definition :
       formations_->formations) {
    if (definition.id == formations_->default_formation) {
      default_definition = &definition;
      break;
    }
  }
  // parse_formations guarantees the default names a defined formation; the
  // defensive fallback keeps explicitly-injected configs from crashing.
  if (default_definition == nullptr) {
    throw std::invalid_argument(
        "formation config: default_formation '" + formations_->default_formation +
        "' is not a defined formation");
  }

  // Formation axes per faction: forward points from the faction's own spawn
  // band center toward its objective (straight across by default); lateral
  // is the perpendicular (forward rotated +90 degrees). Degenerate axes
  // (objective on the band center) fall back to straight across.
  const double band = world_.width * config_.spawn_band_fraction;
  for (Faction faction : kAllFactions) {
    const std::size_t index = faction_index(faction);
    const double band_center_x =
        faction == Faction::Red ? band / 2.0 : world_.width - band / 2.0;
    double forward_x = objective_x_[index] - band_center_x;
    double forward_y = objective_y_[index] - world_.height / 2.0;
    double length = std::sqrt(forward_x * forward_x + forward_y * forward_y);
    if (!(length > 0.0)) {
      forward_x = faction == Faction::Red ? 1.0 : -1.0;
      forward_y = 0.0;
      length = 1.0;
    }
    forward_x_[index] = forward_x / length;
    forward_y_[index] = forward_y / length;
    lateral_x_[index] = -forward_y_[index];
    lateral_y_[index] = forward_x_[index];
  }

  unit_squad_.assign(world_.units.size(), -1);
  unit_member_.assign(world_.units.size(), 0);
  std::uint32_t next_squad_id = 0;
  for (Faction faction : kAllFactions) {
    const std::size_t index = faction_index(faction);
    const auto& plan = config_.squad_plans[index];
    if (!plan) {
      continue;
    }
    // Split the faction's units into commanders and soldiers, then sort
    // both by (y, x, id): squads are spatially contiguous groups in this
    // deterministic order (spawn order ids break exact position ties).
    std::vector<std::uint32_t> soldiers;
    std::vector<std::uint32_t> commanders;
    for (std::size_t i = 0; i < world_.units.size(); ++i) {
      const Unit& unit = world_.units[i];
      if (unit.faction != faction) {
        continue;
      }
      if (unit.stats.leadership_radius > 0.0) {
        commanders.push_back(static_cast<std::uint32_t>(i));
      } else {
        soldiers.push_back(static_cast<std::uint32_t>(i));
      }
    }
    const auto by_position = [this](std::uint32_t a, std::uint32_t b) {
      const Unit& first = world_.units[a];
      const Unit& second = world_.units[b];
      if (first.y != second.y) {
        return first.y < second.y;
      }
      if (first.x != second.x) {
        return first.x < second.x;
      }
      return first.id < second.id;
    };
    std::sort(soldiers.begin(), soldiers.end(), by_position);
    std::sort(commanders.begin(), commanders.end(), by_position);
    // validate_config guarantees the composition matches the plan exactly;
    // the world mirrors the composition (spawn or validated explicit state).
    if (commanders.size() != plan->count ||
        soldiers.size() !=
            static_cast<std::size_t>(plan->count) * plan->size) {
      throw std::invalid_argument(
          std::string(to_string(faction)) +
          " squad plan does not match the fielded army: " +
          std::to_string(commanders.size()) + " commanders and " +
          std::to_string(soldiers.size()) + " soldiers for " +
          std::to_string(plan->count) + " squads of " +
          std::to_string(plan->size));
    }
    for (std::uint32_t k = 0; k < plan->count; ++k) {
      Squad squad;
      squad.id = next_squad_id++;
      squad.faction = faction;
      squad.formation = default_definition->kind;
      squad.spacing = default_definition->spacing;
      squad.leadership_radius =
          world_.units[commanders[k]].stats.leadership_radius;
      squad.commander_index = commanders[k];
      squad.command_active = true;
      squad.order = Order::None;  // the first update_squads picks one
      // Members: the commander first (slot 0 — the center of a line, the
      // front of a column), then the squad's soldiers in sorted order.
      SquadMemberState commander_state;
      commander_state.unit_index = commanders[k];
      squad.members.push_back(commander_state);
      for (std::uint32_t j = 0; j < plan->size; ++j) {
        SquadMemberState member;
        member.unit_index = soldiers[static_cast<std::size_t>(k) * plan->size + j];
        squad.members.push_back(member);
      }
      update_squad_metrics(squad);
      for (std::size_t m = 0; m < squad.members.size(); ++m) {
        unit_squad_[squad.members[m].unit_index] =
            static_cast<std::int32_t>(squad.id);
        unit_member_[squad.members[m].unit_index] = static_cast<std::uint32_t>(m);
      }
      squads_.push_back(std::move(squad));
      squad_stats_[index].squads += 1;
    }
  }
  separation_buffer_.reserve(64);
  squad_perceive_buffer_.reserve(64);
}

void Simulation::spawn_armies() {
  const bool composed = uses_composition(config_);
  const double band = world_.width * config_.spawn_band_fraction;
  const ArmySizes sizes = split_armies(config_.total_units);

  std::uint32_t total = 0;
  if (composed) {
    for (const auto& army : config_.composition) {
      for (const TypeCount& entry : army) {
        total += entry.count;
      }
    }
  } else {
    total = config_.total_units;
  }
  world_.units.reserve(total);
  std::uint32_t next_id = 1;

  // Red spawns in the left band, Blue in the right band; y spans the field.
  // Faction order and per-faction spawn order are fixed, so the stream of RNG
  // draws — and therefore every position — depends only on the seed. In
  // composition mode each faction spawns its types in composition (file)
  // order; every unit draws x then y exactly like M1, so an infantry-only
  // composition reproduces M1 positions bit for bit.
  for (Faction faction : kAllFactions) {
    const std::size_t index = faction_index(faction);
    const std::vector<TypeCount>& entries = config_.composition[index];

    std::uint32_t to_spawn = 0;
    if (composed) {
      for (const TypeCount& entry : entries) {
        to_spawn += entry.count;
      }
    } else {
      to_spawn = faction == Faction::Red ? sizes.red : sizes.blue;
    }

    std::size_t entry_index = 0;
    std::uint32_t drawn_from_entry = 0;
    for (std::uint32_t i = 0; i < to_spawn; ++i) {
      const UnitStats* stats = nullptr;
      std::uint32_t type_index = 0;
      if (composed) {
        // Advance to the next entry once the current one is exhausted
        // (entries with count 0 are rejected by validation).
        while (entry_index < entries.size() &&
               drawn_from_entry >= entries[entry_index].count) {
          ++entry_index;
          drawn_from_entry = 0;
        }
        const TypeCount& entry = entries[entry_index];
        stats = &config_.unit_types->types[entry.type_index].stats;
        type_index = entry.type_index;
        ++drawn_from_entry;
      } else {
        stats = faction == Faction::Red ? &config_.red_stats
                                        : &config_.blue_stats;
      }

      Unit unit;
      unit.id = next_id++;
      unit.faction = faction;
      unit.type_index = type_index;
      // M10: --swap-sides flips which band a faction holds; spawn patterns
      // other than Band replace the RNG draw with deterministic formation
      // math (counter RNG channel 5 for cluster jitter) — the spawn RNG
      // stream itself is only ever touched by the default Band pattern,
      // exactly as in M1-M9.
      const SpawnPattern pattern = config_.spawn_patterns[index];
      const bool west_band = (faction == Faction::Red) != config_.swap_sides;
      const double band_lo = west_band ? 0.0 : world_.width - band;
      const double band_hi = west_band ? band : world_.width;
      if (pattern == SpawnPattern::Band) {
        unit.x = band_lo + rng_.next_double() * band;
        unit.y = rng_.next_double() * world_.height;
      } else {
        // Deterministic pattern placement inside the band (docs/ai.md).
        const std::uint32_t n = to_spawn > 1 ? to_spawn : 1;
        const double t = static_cast<double>(i) / static_cast<double>(n - 1);
        if (pattern == SpawnPattern::Line) {
          unit.x = (band_lo + band_hi) / 2.0;
          unit.y = world_.height * (0.10 + 0.80 * t);
        } else if (pattern == SpawnPattern::Columns) {
          const std::uint32_t per_col = (to_spawn + 3) / 4;
          const std::uint32_t col = i / per_col;
          const std::uint32_t row = i % per_col;
          const std::uint32_t rows = per_col > 1 ? per_col : 1;
          unit.x = band_lo + (0.5 + col) * band / 4.0;
          unit.y = world_.height *
                   (0.10 + 0.80 * static_cast<double>(row) / rows);
        } else if (pattern == SpawnPattern::Echelon) {
          const std::uint32_t rank = i / 8;
          const std::uint32_t pos = i % 8;
          // Stair-steps toward the enemy; the fraction is capped so deep
          // ranks never spill out of the band.
          const double fraction =
              std::min(1.0, 0.15 + 0.70 * pos / 8.0 + 0.08 * rank / 8.0);
          unit.x = band_lo + band * fraction;
          unit.y = world_.height * (0.85 - 0.75 * t);
        } else {  // Cluster: five groups at counter-RNG band centers.
          const std::uint32_t cluster = i % 5;
          const double cx = band_lo + band *
              tactics_roll(config_.seed, 1000 + cluster, 0, 5);
          const double cy = world_.height *
              (0.15 + 0.70 * tactics_roll(config_.seed, 2000 + cluster, 0, 5));
          const double jx = (tactics_roll(config_.seed, unit.id, 0, 5) - 0.5) *
                            band * 0.5;
          const double jy = (tactics_roll(config_.seed, unit.id, 1, 5) - 0.5) *
                            band * 0.5;
          unit.x = std::clamp(cx + jx, band_lo, band_hi);
          unit.y = std::clamp(cy + jy, 0.0, world_.height);
        }
        unit.x = std::clamp(unit.x, 1.0, world_.width - 1.0);
        unit.y = std::clamp(unit.y, 1.0, world_.height - 1.0);
      }
      if (terrain_ != nullptr) {
        // M8 invariant: no unit ever stands on an impassable cell. Redraw
        // (bounded, same RNG stream) while the draw is impassable, then
        // snap to the nearest passable cell center by a fixed ring scan.
        // Pattern spawns never redraw from the stream — they only snap.
        for (int attempt = 0;
             attempt < 64 && pattern == SpawnPattern::Band &&
             !terrain_->passable_at(unit.x, unit.y);
             ++attempt) {
          unit.x = band_lo + rng_.next_double() * band;
          unit.y = rng_.next_double() * world_.height;
        }
        if (!terrain_->passable_at(unit.x, unit.y) &&
            !snap_to_passable(unit.x, unit.y)) {
          throw std::invalid_argument(
              "no passable spawn cell found for a unit (spawn band is "
              "impassable?)");
        }
      }
      unit.health = stats->max_health;
      unit.stats = *stats;
      unit.alive = true;
      world_.units.push_back(unit);
    }
  }
}

void Simulation::tick() {
  if (finished_) {
    return;
  }
  if (terrain_ != nullptr) {
    refresh_flow_fields();
  }
  resolve_cooldowns();
  decide_targets();
  resolve_movement();
  resolve_combat();
  apply_deaths();
  update_morale();
  update_squads();
  update_termination();
  if (config_.timeline_interval > 0 &&
      world_.tick_count % config_.timeline_interval == 0) {
    sample_timeline();
  }
}

bool Simulation::intervene_kill(std::uint32_t unit_id) {
  for (Unit& unit : world_.units) {
    if (unit.id == unit_id) {
      if (!unit.alive || unit.health <= 0.0) {
        return false;
      }
      unit.health = 0.0;
      if (events_ != nullptr) {
        events_->record(world_.tick_count + 1, EventType::Death, unit.id, 0);
      }
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Player orders (m11b). The order travels through the commander system:
// courier delay from the faction HQ to the squad, succession/leaderless
// surcharges, delivery into the squad's order slot, and bounded score
// terms (never overrides). With no order ever issued none of this runs
// and battles are byte-identical to the pre-m11b binary.
// ---------------------------------------------------------------------------

bool Simulation::issue_order(
    Faction faction, const std::vector<std::uint32_t>& squad_ids,
    PlayerOrderType type, double x, double y, const std::string& formation) {
  const std::size_t faction_index_value = faction_index(faction);
  PlayerOrderReport& report = player_order_reports_[faction_index_value];
  const int type_value = static_cast<int>(type);
  if (!config_.orders_enabled || !squads_active_ || squad_ids.empty() ||
      type_value < 0 ||
      type_value > static_cast<int>(PlayerOrderType::ChangeFormation)) {
    report.rejected += squad_ids.size();
    return false;
  }
  // Validate everything before touching state: a wrong-faction or
  // unknown squad id, an unformattable formation or an unsnappable
  // point rejects the whole call.
  for (std::uint32_t squad_id : squad_ids) {
    if (squad_id >= squads_.size() ||
        squads_[squad_id].faction != faction) {
      report.rejected += squad_ids.size();
      return false;
    }
  }
  FormationKind formation_kind = FormationKind::Line;
  double formation_spacing = 3.0;
  if (type == PlayerOrderType::ChangeFormation) {
    bool found = false;
    for (const FormationConfig::Definition& definition :
         formations_->formations) {
      if (definition.id == formation) {
        formation_kind = definition.kind;
        formation_spacing = definition.spacing;
        found = true;
        break;
      }
    }
    if (!found) {
      report.rejected += squad_ids.size();
      return false;
    }
  }
  // A Retreat carries a point exactly when one was given: the (0, 0)
  // default means "fall back toward the own side" (the M6 semantics);
  // a point retreat is a fighting withdrawal to that point.
  const bool point_order = type == PlayerOrderType::MoveTo ||
                           type == PlayerOrderType::AttackMove ||
                           (type == PlayerOrderType::Retreat &&
                            (x != 0.0 || y != 0.0));
  double point_x = x;
  double point_y = y;
  if (point_order && terrain_ != nullptr) {
    if (!terrain_->in_bounds(point_x, point_y) ||
        !snap_to_passable(point_x, point_y)) {
      report.rejected += squad_ids.size();
      return false;
    }
  } else if (point_order) {
    // m12 audit: flat maps validated nothing — negative, off-map and
    // non-finite points were accepted and recorded into replays.
    if (!std::isfinite(point_x) || !std::isfinite(point_y) ||
        point_x < 0.0 || point_x > world_.width ||
        point_y < 0.0 || point_y > world_.height) {
      report.rejected += squad_ids.size();
      return false;
    }
  }
  // Rate limit: at most max_per_tick successful calls per faction per
  // tick (issue_order runs between ticks; the counter resets in
  // update_termination at the end of every tick).
  if (orders_issued_this_tick_[faction_index_value] >=
      commanders_->player_orders.max_per_tick) {
    report.rejected += squad_ids.size();
    return false;
  }

  const std::uint64_t issue_tick = world_.tick_count + 1;
  const std::uint64_t order_type_value = static_cast<std::uint64_t>(
      static_cast<int>(type));
  for (std::uint32_t squad_id : squad_ids) {
    const Squad& squad = squads_[squad_id];
    // Courier distance: HQ -> acting commander, or the squad centroid
    // when command is lost.
    double to_x = squad.centroid_x;
    double to_y = squad.centroid_y;
    if (squad.command_active) {
      to_x = world_.units[squad.commander_index].x;
      to_y = world_.units[squad.commander_index].y;
    }
    const double dx = to_x - hq_x_[faction_index_value];
    const double dy = to_y - hq_y_[faction_index_value];
    std::uint64_t delay = static_cast<std::uint64_t>(std::ceil(
        std::sqrt(dx * dx + dy * dy) * commanders_->order_delay_per_unit));
    // A lost command means the order goes through the successor (the
    // succession surcharge); with no eligible member at issue time the
    // order crawls to whoever is left, later and weaker.
    if (!squad.command_active) {
      bool eligible = false;
      for (const SquadMemberState& member : squad.members) {
        const Unit& candidate = world_.units[member.unit_index];
        if (candidate.alive && !candidate.routed) {
          eligible = true;
          break;
        }
      }
      delay += eligible ? commanders_->succession_delay_ticks
                        : commanders_->player_orders.leaderless_delay_ticks;
    }
    pending_orders_.push_back(PlayerOrder{
        issue_tick, issue_tick + delay, faction, squad_id, type,
        point_x, point_y, point_order,
        type == PlayerOrderType::ChangeFormation,
        formation_kind, formation_spacing, 0.0});
    report.issued += 1;
    if (events_ != nullptr) {
      events_->record(issue_tick, EventType::OrderIssued, squad_id,
                      static_cast<std::uint32_t>(order_type_value));
    }
  }
  orders_issued_this_tick_[faction_index_value] += 1;
  // Keep the queue deliverable order stable: ascending deliver tick,
  // then issue order (push_back order).
  std::stable_sort(pending_orders_.begin(), pending_orders_.end(),
                   [](const PlayerOrder& a, const PlayerOrder& b) {
                     return a.deliver_tick < b.deliver_tick;
                   });
  return true;
}

void Simulation::clear_player_order(Squad& squad) {
  squad.player_order_active = false;
  squad.player_order_point = false;
  squad.player_order_x = 0.0;
  squad.player_order_y = 0.0;
  if (squad.id < squad_flow_.size()) {
    squad_flow_[squad.id] = FlowField{};
    squad_flow_target_[squad.id] =
        std::numeric_limits<std::uint64_t>::max();
  }
}

void Simulation::apply_player_order(Squad& squad, const PlayerOrder& order,
                                    std::uint64_t tick_number) {
  // Map the player order onto the commander-order slot; the member
  // propagation (commander -> member, distance delay) is the standard
  // M6 machinery, except for leaderless squads where there is no
  // commander to propagate: the order reaches every member directly at
  // reduced obedience (the reduced-strength rule).
  Order mapped = Order::Advance;
  switch (order.type) {
    case PlayerOrderType::MoveTo:
    case PlayerOrderType::AttackMove:
      mapped = Order::Advance;
      break;
    case PlayerOrderType::Hold:
      mapped = Order::Hold;
      break;
    case PlayerOrderType::Retreat:
      mapped = Order::Retreat;
      break;
    case PlayerOrderType::Regroup:
      mapped = Order::Regroup;
      break;
    case PlayerOrderType::ChangeFormation:
      mapped = squad.order;  // a formation change keeps the current order
      break;
  }
  squad.order = mapped;
  squad.order_issue_tick = tick_number;
  squad.player_order_active = true;
  squad.player_order_type = order.type;
  squad.player_order_point = order.has_point;
  squad.player_order_x = order.x;
  squad.player_order_y = order.y;
  if (!squad.command_active) {
    for (SquadMemberState& member : squad.members) {
      member.order = mapped;
      member.out_of_range_ticks = 0;
    }
  }
  if (order.formation_set) {
    squad.formation = order.formation;
    squad.spacing = order.formation_spacing;
  }
  // A point order retargets the squad's march: compute the squad's own
  // flow field once (the same Dijkstra the faction field uses).
  if (order.has_point && terrain_ != nullptr) {
    const std::uint32_t cx = static_cast<std::uint32_t>(
        order.x / terrain_->cell_size);
    const std::uint32_t cy = static_cast<std::uint32_t>(
        order.y / terrain_->cell_size);
    squad_flow_[squad.id] = compute_flow_field(*terrain_, cx, cy);
    squad_flow_target_[squad.id] =
        static_cast<std::uint64_t>(cy) * terrain_->width + cx;
  } else {
    squad_flow_[squad.id] = FlowField{};
    squad_flow_target_[squad.id] =
        std::numeric_limits<std::uint64_t>::max();
  }
  // Obedience at delivery, recorded for the report: morale, discipline
  // and cohesion (the spec's factors — the distance is spent on the
  // courier delay, not the obedience); leaderless squads are scaled by
  // the leaderless factor. Mirrors the member-level term in
  // decide_unit_action.
  double obedience = 0.0;
  if (squad.command_active) {
    const Unit& commander = world_.units[squad.commander_index];
    const double morale_factor = [&] {
      switch (static_cast<MoraleState>(commander.morale_state)) {
        case MoraleState::Confident:
          return commanders_->obedience.morale_factor_confident;
        case MoraleState::Steady:
          return commanders_->obedience.morale_factor_steady;
        case MoraleState::Worried:
          return commanders_->obedience.morale_factor_worried;
        default:
          return commanders_->obedience.morale_factor_shaken;
      }
    }();
    const double cohesion_term =
        commanders_->obedience.cohesion_floor +
        (1.0 - commanders_->obedience.cohesion_floor) * squad.cohesion;
    obedience = std::clamp(commander.personality.discipline * morale_factor *
                               cohesion_term,
                           0.0, 1.0);
  } else {
    const double cohesion_term =
        commanders_->obedience.cohesion_floor +
        (1.0 - commanders_->obedience.cohesion_floor) * squad.cohesion;
    obedience = std::clamp(commanders_->player_orders.leaderless_obedience *
                               cohesion_term,
                           0.0, 1.0);
  }
  PlayerOrderReport& report =
      player_order_reports_[faction_index(squad.faction)];
  report.delivered += 1;
  report.delay_ticks_sum += order.deliver_tick - order.issue_tick;
  report.obedience_sum += obedience;
  if (events_ != nullptr) {
    events_->record(tick_number, EventType::OrderDelivered, squad.id,
                    static_cast<std::uint32_t>(static_cast<int>(order.type)));
  }
}

void Simulation::process_pending_orders(std::uint64_t tick_number) {
  std::size_t next = 0;
  while (next < pending_orders_.size() &&
         pending_orders_[next].deliver_tick <= tick_number) {
    const PlayerOrder order = pending_orders_[next];
    ++next;
    if (order.squad_id >= squads_.size()) {
      continue;  // defensive: ids are validated at issue time
    }
    Squad& squad = squads_[order.squad_id];
    bool any_alive = false;
    for (const SquadMemberState& member : squad.members) {
      if (world_.units[member.unit_index].alive) {
        any_alive = true;
        break;
      }
    }
    if (!any_alive) {
      player_order_reports_[faction_index(order.faction)].dropped += 1;
      continue;
    }
    apply_player_order(squad, order, tick_number);
  }
  pending_orders_.erase(pending_orders_.begin(),
                        pending_orders_.begin() +
                            static_cast<std::ptrdiff_t>(next));
}

bool Simulation::order_flow_step(std::size_t unit_index, std::size_t faction,
                                 double x, double y, double& dx,
                                 double& dy) const noexcept {
  if (squads_active_ && unit_index < unit_squad_.size() &&
      unit_squad_[unit_index] >= 0) {
    const Squad& squad = squads_[static_cast<std::size_t>(
        unit_squad_[unit_index])];
    if (squad.player_order_point &&
        static_cast<std::size_t>(squad.id) < squad_flow_.size() &&
        squad_flow_[squad.id].target_valid) {
      const std::uint32_t cx =
          static_cast<std::uint32_t>(x / terrain_->cell_size);
      const std::uint32_t cy =
          static_cast<std::uint32_t>(y / terrain_->cell_size);
      if (flow_direction(squad_flow_[squad.id], cx, cy, dx, dy)) {
        return true;
      }
      return false;  // on or past the squad objective: no faction fallback
    }
  }
  return flow_step(faction, x, y, dx, dy);
}

// M10 Part 2a: one timeline row for the current tick. Read-only.
void Simulation::sample_timeline() {
  TimelineRow row;
  row.tick = world_.tick_count;
  const std::size_t type_count =
      config_.unit_types != nullptr ? config_.unit_types->types.size()
                                  : 1;
  for (FactionTimeline& side : row.factions) {
    side.alive_by_type.assign(type_count, 0);
  }
  for (std::size_t i = 0; i < world_.units.size(); ++i) {
    const Unit& unit = world_.units[i];
    if (!unit.alive) {
      continue;
    }
    FactionTimeline& side = row.factions[faction_index(unit.faction)];
    ++side.alive;
    if (unit.routed) {
      ++side.routed;
    } else {
      side.morale_sum += morale_enabled_ ? unit.morale : 50.0;
      ++side.morale_n;
    }
    if (unit.last_action == static_cast<std::uint8_t>(Action::Attack)) {
      ++side.engagements;
    }
    ++side.alive_by_type[unit.type_index];
  }
  for (std::size_t f = 0; f < kFactionCount; ++f) {
    row.factions[f].dead = faction_reports_[f].deaths;
  }
  timeline_rows_.push_back(row);
}

BattleResult Simulation::run() {
  while (!finished_) {
    tick();
  }
  if (config_.timeline_interval > 0 &&
      (timeline_rows_.empty() ||
       timeline_rows_.back().tick != world_.tick_count)) {
    sample_timeline();
  }
  return result();
}

BattleResult Simulation::result() const {
  BattleResult out;
  out.finished = finished_;
  out.reason = reason_;
  out.has_winner = has_winner_;
  out.winner = winner_;
  out.ticks_elapsed = world_.tick_count;
  for (Faction faction : kAllFactions) {
    FactionReport& report = out.factions[faction_index(faction)];
    report.initial = faction_reports_[faction_index(faction)].initial;
    report.deaths = faction_reports_[faction_index(faction)].deaths;
    report.survivors = world_.alive_count(faction);
    report.action_counts = action_counts_[faction_index(faction)];
    report.routing = routing_stats_[faction_index(faction)];
    report.squad = squad_stats_[faction_index(faction)];
    report.combat = faction_reports_[faction_index(faction)].combat;
  }
  // Active units (M5): alive and not routed. With morale off (and in legacy
  // mode) routed is always false, so active == survivors.
  for (const Unit& unit : world_.units) {
    if (unit.alive && !unit.routed) {
      out.factions[faction_index(unit.faction)].active += 1;
    }
  }
  if (use_type_reports_) {
    for (Faction faction : kAllFactions) {
      const std::size_t index = faction_index(faction);
      out.factions[index].by_type = type_reports_[index];
    }
    for (const Unit& unit : world_.units) {
      if (unit.alive) {
        out.factions[faction_index(unit.faction)]
            .by_type[unit.type_index]
            .survivors += 1;
      }
    }
    // Types that never spawned carry no meaningful statistics.
    for (FactionReport& report : out.factions) {
      std::erase_if(report.by_type,
                    [](const TypeReport& type) { return type.spawned == 0; });
    }
  }
  if (terrain_ != nullptr) {
    for (std::uint16_t stretch : max_stuck_stretch_) {
      if (stretch > out.longest_stuck_stretch) {
        out.longest_stuck_stretch = stretch;
      }
    }
    out.units_stuck_over_100 = units_stuck_over_100_;
    out.terrain_kills = terrain_kills_;
  }
  out.first_contact_tick = first_contact_tick_;
  return out;
}

// Phase 0 — timers. Cooldowns tick down at the start of each tick, so
// `attack_cooldown_ticks == K` means exactly K ticks between two attacks.
void Simulation::resolve_cooldowns() {
  for (Unit& unit : world_.units) {
    if (unit.alive && unit.cooldown_remaining > 0) {
      --unit.cooldown_remaining;
    }
  }
}

// Phase 1 — perception and decisions. Reads positions only; every unit
// decides against the same snapshot of the world.
//
// Legacy mode (M1-M3): every unit targets its nearest living enemy; grid and
// brute neighbor modes return the same target by construction (exact ring
// expansion with the brute-force tie-break); the brute path stays available
// as the oracle.
//
// Utility mode (M4): perception is limited to the unit's sight radius (grid
// radius query over the snapshot), actions are scored and the best is chosen
// (docs/ai.md). Dead units keep their last decision but are never targeted.
void Simulation::decide_targets() {
  target_index_.assign(world_.units.size(), -1);
  if (config_.ai_mode == AiMode::Utility) {
    // M10: clear this tick's tactic goals and mark which squads had a
    // member seeing an enemy on the previous tick (a 1-tick-stale but
    // deterministic "frontally engaged" signal for Flank).
    if (config_.ai_avail_v2) {
      // m13: the per-ally Help cap counts within one tick.
      std::fill(help_target_count_.begin(), help_target_count_.end(), 0u);
    }
    if (tactics_active_) {
      std::fill(tactic_goal_set_.begin(), tactic_goal_set_.end(), 0);
      if (squads_active_) {
        std::fill(squad_engaged_.begin(), squad_engaged_.end(), 0);
        for (std::size_t i = 0; i < world_.units.size(); ++i) {
          if (world_.units[i].alive && unit_squad_[i] >= 0 &&
              last_perception_[i].visible_enemies > 0) {
            squad_engaged_[static_cast<std::size_t>(unit_squad_[i])] = 1;
          }
        }
      }
    }
    for (std::size_t i = 0; i < world_.units.size(); ++i) {
      if (!world_.units[i].alive) {
        continue;  // target stays -1; last_action/perception keep their values
      }
      Perception perception;
      ActionScores scores{};
      const Action action = decide_unit_action(i, perception, scores);
      last_perception_[i] = perception;
      if (perception.visible_enemies > 0 && first_contact_tick_ == 0) {
        // m11a: the first enemy sighting of the battle (report field +
        // FirstContact event). Record the seer's id; deterministic
        // because the unit loop is in index order.
        first_contact_tick_ = world_.tick_count + 1;
        if (events_ != nullptr) {
          events_->record(first_contact_tick_, EventType::FirstContact,
                          world_.units[i].id);
        }
      }
      if (events_ != nullptr) {
        const std::uint8_t previous = world_.units[i].last_action;
        const std::uint8_t now = static_cast<std::uint8_t>(action);
        if (now == static_cast<std::uint8_t>(Action::Flank) &&
            previous != now) {
          events_->record(world_.tick_count + 1, EventType::FlankStart,
                          world_.units[i].id);
        } else if (now == static_cast<std::uint8_t>(Action::Help) &&
                   previous != now) {
          events_->record(world_.tick_count + 1, EventType::HelpStart,
                          world_.units[i].id);
        }
      }
      world_.units[i].last_action = static_cast<std::uint8_t>(action);
      target_index_[i] = perception.nearest_visible_enemy;
      // Flee is a forced routing behavior, not a scored action; it is
      // tracked by the per-faction routing statistics instead.
      if (static_cast<std::size_t>(action) < kActionCount) {
        action_counts_[faction_index(world_.units[i].faction)]
                      [static_cast<std::size_t>(action)] += 1;
      }
    }
    update_squad_contacts();
    update_faction_estimates();
    return;
  }
  if (use_grid_) {
    for (std::size_t i = 0; i < world_.units.size(); ++i) {
      const Unit& unit = world_.units[i];
      if (!unit.alive) {
        continue;
      }
      target_index_[i] = grid_.nearest_enemy(world_.units, unit);
    }
    return;
  }
  for (std::size_t i = 0; i < world_.units.size(); ++i) {
    const Unit& unit = world_.units[i];
    if (!unit.alive) {
      continue;
    }
    target_index_[i] = nearest_enemy_index(world_.units, unit);
  }
}

// Utility AI (M4) for one unit, entirely from the tick-start snapshot: the
// grid is current as of the end of the previous tick (or construction), and
// positions do not change between this phase and the movement commit.
Action Simulation::decide_unit_action(std::size_t index,
                                       Perception& perception,
                                       ActionScores& scores) {
  const Unit& unit = world_.units[index];
  const double sight = effective_sight(unit);

  perception = Perception{};
  grid_.radius_query(world_.units, unit.x, unit.y, sight, perceive_buffer_);

  const Faction enemy =
      unit.faction == Faction::Red ? Faction::Blue : Faction::Red;
  int best = -1;
  double best_d2 = 0.0;
  double sum_x = 0.0;
  double sum_y = 0.0;
  std::uint32_t enemies = 0;
  std::uint32_t allies = 0;
  std::uint32_t routed_allies = 0;
  for (std::uint32_t candidate : perceive_buffer_) {
    if (candidate == index) {
      continue;  // a unit is not its own ally
    }
    const Unit& other = world_.units[candidate];
    if (other.faction == enemy) {
      ++enemies;
      sum_x += other.x;
      sum_y += other.y;
      const double dx = other.x - unit.x;
      const double dy = other.y - unit.y;
      const double d2 = dx * dx + dy * dy;
      // The M1 targeting rule inside the sight limit: nearest squared
      // distance, exact ties to the lower unit id (scan-order independent).
      if (best < 0 || d2 < best_d2 ||
          (d2 == best_d2 &&
           other.id < world_.units[static_cast<std::size_t>(best)].id)) {
        best = static_cast<int>(candidate);
        best_d2 = d2;
      }
    } else {
      ++allies;
      if (morale_enabled_ && other.routed) {
        ++routed_allies;
      }
    }
  }
  perception.nearest_visible_enemy = best;
  perception.nearest_visible_enemy_distance =
      best >= 0 ? std::sqrt(best_d2) : 0.0;
  perception.visible_enemies = enemies;
  perception.visible_allies = allies;
  perception.routed_allies = routed_allies;
  if (enemies > 0) {
    perception.enemy_centroid_x = sum_x / enemies;
    perception.enemy_centroid_y = sum_y / enemies;
  }

  // M10 tactical state (tactics on only; see docs/ai.md). Personal memory
  // records the nearest visible enemy; the freshest usable contact (own
  // memory, or squad intel read inside the commander's radius once the
  // share delay has passed) feeds Search.
  double search_goal_x = 0.0;
  double search_goal_y = 0.0;
  bool has_fresh_contact = false;
  double contact_distance = 0.0;
  double contact_age = 0.0;
  bool ally_in_need = false;
  double ally_health_fraction = 1.0;
  double ally_distance = 0.0;
  std::size_t ally_goal = 0;
  bool squad_engaged = false;
  bool member_may_flank = false;
  double squad_cohesion = 1.0;
  bool has_squad = false;
  if (tactics_active_) {
    const std::uint64_t tick_number = world_.tick_count + 1;
    if (best >= 0) {
      const Unit& enemy = world_.units[static_cast<std::size_t>(best)];
      world_.units[index].memory_x = enemy.x;
      world_.units[index].memory_y = enemy.y;
      world_.units[index].memory_tick = tick_number;
    }
    std::uint64_t best_tick = 0;
    double best_x = 0.0;
    double best_y = 0.0;
    if (world_.units[index].memory_tick > 0) {
      best_tick = world_.units[index].memory_tick;
      best_x = world_.units[index].memory_x;
      best_y = world_.units[index].memory_y;
    }
    if (squads_active_ && unit_squad_[index] >= 0) {
      const Squad& squad = squads_[static_cast<std::size_t>(
          unit_squad_[index])];
      has_squad = true;
      squad_cohesion = squad.cohesion;
      squad_engaged = squad_engaged_[static_cast<std::size_t>(
          unit_squad_[index])] != 0;
      const std::uint32_t member_index = unit_member_[index];
      member_may_flank =
          member_index >= 1 &&
          member_index <= effective_tactics_->flank.max_per_squad;
      if (squad.command_active) {
        const Unit& commander = world_.units[squad.commander_index];
        const double cdx = unit.x - commander.x;
        const double cdy = unit.y - commander.y;
        const double radius = squad.leadership_radius;
        if (cdx * cdx + cdy * cdy <= radius * radius) {
          // Squad intel: readable once the share delay has passed.
          for (std::size_t c = 0; c < squad.contact_count; ++c) {
            const SquadContact& contact = squad.contacts[c];
            if (contact.tick + effective_tactics_->memory
                                      .squad_intel_delay_ticks <=
                    tick_number &&
                contact.tick > best_tick) {
              best_tick = contact.tick;
              best_x = contact.x;
              best_y = contact.y;
            }
          }
        }
      }
    }
    // Contact seeking (M10 continuation): a unit that has seen no enemy
    // for idle_ticks consecutive ticks escalates its freshest contact to
    // the faction estimate (while that is fresh enough), so Search heads
    // for where the enemy was last seen by anyone instead of standing on
    // a stale personal memory. The streak resets on any sighting.
    if (contact_seek_active_) {
      const std::size_t faction = faction_index(unit.faction);
      if (best >= 0) {
        contactless_streak_[index] = 0;
      } else if (contactless_streak_[index] <=
                 effective_tactics_->contact_seek.idle_ticks) {
        contactless_streak_[index] += 1;
      }
      bool seeking_now =
          best < 0 &&
          contactless_streak_[index] >=
              effective_tactics_->contact_seek.idle_ticks &&
          estimate_tick_[faction] > 0 &&
          tick_number - estimate_tick_[faction] <=
              effective_tactics_->contact_seek.estimate_max_age;
      // m13 item 2 (--ai-avail v2): a seeker that has reached the
      // estimated position and still sees nothing proves the estimate
      // stale — drop it (the next real sighting refreshes it) instead
      // of milling around the point forever.
      if (config_.ai_avail_v2 && seeking_now) {
        const double edx = unit.x - estimate_x_[faction];
        const double edy = unit.y - estimate_y_[faction];
        if (edx * edx + edy * edy <= sight * sight) {
          estimate_tick_[faction] = 0;
          seeking_now = false;
        }
      }
      if (events_ != nullptr && seeking_now && !was_seeking_[index]) {
        events_->record(tick_number, EventType::SeekStart, unit.id);
      }
      was_seeking_[index] = seeking_now ? 1 : 0;
      if (seeking_now) {
        best_tick = estimate_tick_[faction];
        best_x = estimate_x_[faction];
        best_y = estimate_y_[faction];
      }
    }
    if (best_tick > 0) {
      contact_age = static_cast<double>(tick_number - best_tick);
      if (contact_age <
          static_cast<double>(effective_tactics_->memory.forget_ticks)) {
        has_fresh_contact = true;
        search_goal_x = best_x;
        search_goal_y = best_y;
        const double dx = best_x - unit.x;
        const double dy = best_y - unit.y;
        contact_distance = std::sqrt(dx * dx + dy * dy);
      }
    }
    // Help: the neediest visible ally inside help.radius — hurt below the
    // threshold or damaged on the previous tick (damage_this_tick_ still
    // holds that tick's combat at decide time).
    for (std::uint32_t candidate : perceive_buffer_) {
      if (candidate == index) {
        continue;
      }
      const Unit& other = world_.units[candidate];
      if (other.faction != unit.faction || !other.alive) {
        continue;
      }
      const double fraction =
          std::clamp(other.health / other.stats.max_health, 0.0, 1.0);
      const bool in_need =
          fraction < effective_tactics_->help.ally_health_below ||
          damage_this_tick_[candidate] > 0.0;
      if (!in_need) {
        continue;
      }
      // m13 item 2 (--ai-avail v2): a routed ally far from any living
      // enemy is out of the fight, not in danger — never a Help target.
      if (config_.ai_avail_v2 && other.routed) {
        const double reach =
            effective_tactics_->help.routed_help_enemy_distance;
        grid_.radius_query(world_.units, other.x, other.y, reach,
                           help_probe_buffer_);
        bool near_enemy = false;
        for (std::uint32_t probe : help_probe_buffer_) {
          const Unit& probe_unit = world_.units[probe];
          if (probe_unit.alive && probe_unit.faction != unit.faction) {
            near_enemy = true;
            break;
          }
        }
        if (!near_enemy) {
          continue;
        }
      }
      const double dx = other.x - unit.x;
      const double dy = other.y - unit.y;
      const double d2 = dx * dx + dy * dy;
      const double radius = effective_tactics_->help.radius;
      if (d2 > radius * radius) {
        continue;
      }
      const double distance = std::sqrt(d2);
      if (!ally_in_need || fraction < ally_health_fraction - 1e-12 ||
          (fraction == ally_health_fraction &&
           candidate < static_cast<std::uint32_t>(ally_goal))) {
        ally_in_need = true;
        ally_health_fraction = fraction;
        ally_distance = distance;
        ally_goal = candidate;
      }
    }
  }

  // Every score input is clamped to [0, 1] so scores stay finite and in
  // [0, 1] by construction (see docs/ai.md).
  ScoreInputs inputs;
  inputs.health_fraction =
      std::clamp(unit.health / unit.stats.max_health, 0.0, 1.0);
  inputs.strength_ratio = static_cast<double>(allies + 1) /
                          static_cast<double>(allies + enemies + 1);
  inputs.distance_fraction =
      best >= 0 ? std::clamp(std::sqrt(best_d2) / sight, 0.0, 1.0) : 0.0;
  inputs.cooldown_readiness =
      std::clamp(1.0 - static_cast<double>(unit.cooldown_remaining) /
                           static_cast<double>(unit.stats
                                                   .attack_cooldown_ticks),
                 0.0, 1.0);
  inputs.bravery = std::clamp(unit.personality.bravery, 0.0, 1.0);
  inputs.caution = std::clamp(unit.personality.caution, 0.0, 1.0);
  inputs.aggression = std::clamp(unit.personality.aggression, 0.0, 1.0);
  inputs.has_visible_enemy = best >= 0;
  inputs.enemy_in_attack_range =
      best >= 0 &&
      best_d2 <= unit.stats.attack_range * unit.stats.attack_range;
  // M10 inputs (zeros/defaults with tactics off, so legacy scoring and
  // every M1-M9 reference are untouched).
  inputs.sight_radius = sight;
  // m13 item 2: --ai-avail v2 gates the regroup lift in score_actions.
  inputs.availability_v2 = config_.ai_avail_v2;
  inputs.has_fresh_contact = has_fresh_contact;
  inputs.contact_distance = contact_distance;
  inputs.contact_age = contact_age;
  inputs.ally_in_need = ally_in_need;
  inputs.ally_health_fraction = ally_health_fraction;
  inputs.ally_distance = ally_distance;
  inputs.squad_engaged = squad_engaged;
  inputs.member_may_flank = member_may_flank;
  inputs.local_enemy_ratio =
      std::min(99.0, static_cast<double>(allies + 1) /
                         static_cast<double>(enemies + 1));
  inputs.target_routed = best >= 0 && world_.units[static_cast<std::size_t>(best)].routed;
  inputs.unit_speed = unit.stats.speed;
  {
    const double adx = unit.x - unit.pursuit_anchor_x;
    const double ady = unit.y - unit.pursuit_anchor_y;
    inputs.pursuit_within_bounds =
        inputs.target_routed &&
        unit.pursuit_ticks < effective_tactics_->pursuit.max_ticks &&
        (unit.pursuit_ticks == 0 ||
         adx * adx + ady * ady <
             effective_tactics_->pursuit.max_distance *
                 effective_tactics_->pursuit.max_distance);
  }
  inputs.has_squad = has_squad;
  inputs.squad_cohesion = squad_cohesion;
  // Pursuit disengagement (M10): past the time or distance bound a routed
  // enemy stops driving movement — for scoring it is as if no enemy were
  // visible (an adjacent one can still be attacked), and the unit's
  // memory of it is suppressed this tick.
  const bool pursuit_expired =
      tactics_active_ && inputs.target_routed &&
      !inputs.pursuit_within_bounds;
  if (pursuit_expired) {
    if (!inputs.enemy_in_attack_range) {
      inputs.has_visible_enemy = false;
      inputs.enemy_in_attack_range = false;
    }
    has_fresh_contact = false;
  }

  scores = score_actions(inputs, tactics_active_ ? effective_tactics_
                                                 : nullptr);
  const Action previous = static_cast<Action>(unit.last_action);
  // The order term (M6), captured for --ai-debug even when zero.
  std::array<double, kActionCount> order_bonus{};
  double obedience = 0.0;
  Action chosen;
  // Broken units (M5) do not score: they flee, cannot Advance, Approach or
  // attack, and the commit time does not apply (they stay routed until the
  // recovery conditions are met in update_morale). A unit whose morale is
  // below the breaking value flees immediately, even on the same tick it
  // crossed the threshold; the routed flag records the break for the
  // hysteresis and statistics machinery.
  const bool broken =
      morale_enabled_ &&
      (unit.routed || unit.morale < effective_morale_->states.shaken_min);
  if (broken) {
    scores.fill(-1.0);
    chosen = Action::Flee;
    action_commit_[index] = 0;
  } else {
    if (morale_enabled_) {
      // Morale-state score modifiers (docs/morale.md): Worried/Shaken lower
      // attack and approach and raise retreat; Confident does the opposite.
      apply_morale_modifiers(
          scores, static_cast<MoraleState>(unit.morale_state),
          effective_morale_->ai);
    }
    // The commander's order as a bounded score term (M6): obedience *
    // order weight on the actions the order favors, only where the action
    // is available, then clamped to [0, 1]. Never an override — it nudges
    // scores, availability rules stay untouched.
    // m11b: a delivered player order applies the same term to a
    // leaderless squad (no commander to propagate — delivered directly
    // at reduced obedience), and MoveTo adds a small keep-marching bias
    // on Hold while enemies are visible.
    bool player_order_scored = false;
    if (squads_active_ && unit_squad_[index] >= 0) {
      const Squad& squad = squads_[unit_squad_[index]];
      const SquadMemberState& member =
          squad.members[unit_member_[index]];
      if (member.order != Order::None &&
          (squad.command_active || squad.player_order_active)) {
        if (squad.command_active && !squad.player_order_active) {
          // M6 AI order: obedience falls with the member's distance to
          // the commander inside the leadership radius.
          const Unit& commander = world_.units[squad.commander_index];
          const double dx = unit.x - commander.x;
          const double dy = unit.y - commander.y;
          obedience = obedience_weight(
              unit.personality.discipline, std::sqrt(dx * dx + dy * dy),
              squad.leadership_radius,
              static_cast<MoraleState>(unit.morale_state), squad.cohesion,
              *commanders_);
        } else if (squad.player_order_active) {
          // m11b player order: the spec's obedience factors are morale,
          // discipline and cohesion — the distance is spent on the
          // delivery delay (HQ -> commander), so it does not gate the
          // member term again. (Measured with the distance factor in:
          // mean obedience 0.16, a delivered hold order moved 12% of
          // the army — cosmetic.)
          const double morale_factor = [&] {
            switch (static_cast<MoraleState>(unit.morale_state)) {
              case MoraleState::Confident:
                return commanders_->obedience.morale_factor_confident;
              case MoraleState::Steady:
                return commanders_->obedience.morale_factor_steady;
              case MoraleState::Worried:
                return commanders_->obedience.morale_factor_worried;
              default:
                return commanders_->obedience.morale_factor_shaken;
            }
          }();
          const double cohesion_term =
              commanders_->obedience.cohesion_floor +
              (1.0 - commanders_->obedience.cohesion_floor) *
                  squad.cohesion;
          const double scale = squad.command_active
                                   ? 1.0
                                   : commanders_->player_orders
                                         .leaderless_obedience;
          obedience = std::clamp(
              unit.personality.discipline * morale_factor *
                  cohesion_term * scale,
              0.0, 1.0);
        } else {
          // Leaderless AI order path (kept for symmetry): cohesion
          // only.
          obedience = 0.0;
        }
        // m11b strength model: a delivered player order carries the
        // faction's authority, so its delivery strength floors at half
        // the commander-order weight at zero obedience and reaches the
        // full (scaled) weight at full obedience. The bonus is still a
        // bounded score term: clamped to [0, 1] per action, never
        // touching availability, and routed units (forced Flee) never
        // see it.
        double delivery_strength = obedience;
        if (squad.player_order_active) {
          delivery_strength = 0.5 + 0.5 * obedience;
          player_order_scored = true;
        }
        order_bonus = order_score_bonus(member.order, delivery_strength,
                                        commanders_->order_weights);
        if (squad.player_order_active) {
          // m12 audit: docs/squads.md promises "AttackMove leaves
          // engagement scoring untouched" (and MoveTo marches past
          // enemies). The Order::Advance mapping boosts Approach, which
          // at weight_scale 4.0 saturates at 1.0 and outranked Attack:
          // ordered armies crowded the enemy without ever attacking
          // (measured kill ratios 0.026 / 0.003 in the m11 study).
          //
          // There is a second amplifier: the M10 regroup term clamps
          // Advance as clamp(-1 + 0, 0, 1) = 0.0 while enemies are
          // visible (an unavailable action lifted to a pseudo-available
          // 0.0 — harmless without orders, where 0.0 never wins). The
          // order bonus adds to any score >= 0, so even a march-only
          // bonus would score Advance 0.88 over Approach 0.70 and the
          // army would march past the enemy without closing to melee.
          //
          // So: MoveTo keeps the march bonus (Advance + Search — that
          // is what makes it march past enemies; in melee range base
          // Attack ~0.9 still outranks the saturated Advance 0.88, so a
          // MoveTo column defends itself) and loses the Approach boost
          // (it must not chase). AttackMove zeroes the whole bonus: the
          // ordered point still steers the Advance move, and Attack and
          // Approach keep their base scores, so an attack-moving army
          // fights normally on contact.
          if (squad.player_order_type == PlayerOrderType::MoveTo) {
            order_bonus[static_cast<std::size_t>(Action::Approach)] = 0.0;
          } else if (squad.player_order_type ==
                     PlayerOrderType::AttackMove) {
            order_bonus[static_cast<std::size_t>(Action::Approach)] = 0.0;
            order_bonus[static_cast<std::size_t>(Action::Advance)] = 0.0;
            order_bonus[static_cast<std::size_t>(Action::Search)] = 0.0;
          }
          for (double& bonus : order_bonus) {
            bonus *= commanders_->player_orders.weight_scale;
          }
        }
        if (config_.ai_avail_v2 && squad.player_order_active &&
            squad.player_order_type == PlayerOrderType::MoveTo &&
            scores[static_cast<std::size_t>(Action::Advance)] < 0.0) {
          // m13 item 2: the regroup clamp that used to lift an
          // unavailable Advance to a pseudo-available 0.0 is gone under
          // --ai-avail v2, but MoveTo's documented "marches past
          // enemies" needs the march available while enemies are
          // visible — the same 0.0 base, now explicit and scoped to
          // delivered MoveTo orders only (melee still wins: base Attack
          // ~0.9 outranks the saturated Advance bonus of ~0.88).
          scores[static_cast<std::size_t>(Action::Advance)] = 0.0;
        }
        for (std::size_t action = 0; action < kActionCount; ++action) {
          if (scores[action] >= 0.0) {
            scores[action] =
                std::clamp(scores[action] + order_bonus[action], 0.0, 1.0);
          }
        }
        if (squad.player_order_active &&
            squad.player_order_type == PlayerOrderType::MoveTo &&
            scores[static_cast<std::size_t>(Action::Hold)] >= 0.0) {
          scores[static_cast<std::size_t>(Action::Hold)] = std::clamp(
              scores[static_cast<std::size_t>(Action::Hold)] +
                  obedience * commanders_->player_orders.moveto_hold_bias,
              0.0, 1.0);
        }
      }
    }
    if (action_commit_[index] > 0 && previous != Action::None &&
        scores[static_cast<std::size_t>(previous)] >= 0.0) {
      // Minimum commit time: hold the current action for the commit period
      // after a switch, as long as it stays available. This damps score
      // cycles (e.g. the cooldown-readiness dip right after an attack) on
      // top of the stickiness bonus. With tactics on (M10) the commit
      // period is the unit's 1-5 tick decision interval instead of the M4
      // constant, so units react with a personality-staggered delay.
      chosen = previous;
      action_commit_[index] -= 1;
    } else {
      // m12 audit: under a delivered player order, cap the stickiness
      // bonus at the bounded score maximum so saturated actions tie-break
      // by enum priority (Attack first) instead of sticky-locking the
      // current one (a Hold order used to pin Hold over Attack forever).
      chosen = choose_action(scores, previous, config_.action_stickiness,
                             player_order_scored);
      // m13 item 2 (--ai-avail v2): at most help.max_helpers_per_ally
      // units may Help the same ally in one tick; a full cap falls
      // through to the next-best action (unit index order makes the
      // cap deterministic).
      if (config_.ai_avail_v2 && chosen == Action::Help && ally_in_need) {
        if (help_target_count_[ally_goal] >=
            static_cast<std::uint32_t>(std::max(
                0, effective_tactics_->help.max_helpers_per_ally))) {
          scores[static_cast<std::size_t>(Action::Help)] = -1.0;
          chosen = choose_action(scores, previous, config_.action_stickiness,
                                 player_order_scored);
        } else {
          ++help_target_count_[ally_goal];
        }
      }
      if (tactics_active_) {
        const std::uint32_t interval =
            unit.decision_interval > 0 ? unit.decision_interval : 1;
        action_commit_[index] = static_cast<std::uint8_t>(interval - 1);
      } else {
        action_commit_[index] =
            chosen == previous
                ? 0
                : static_cast<std::uint8_t>(kActionCommitTicks);
      }
    }
    if (tactics_active_) {
      // Pursuit bookkeeping (M10): consecutive ticks of pursuing the same
      // routed enemy, with the anchor set when that pursuit starts; the
      // counter is capped at pursuit.max_ticks (never a runaway chase).
      // A different target, or a fighting (non-pursuit) action, freezes
      // or resets the state.
      const bool pursuing = chosen == Action::Approach && inputs.target_routed;
      if (pursuing) {
        const std::uint32_t target_id =
            world_.units[static_cast<std::size_t>(best)].id;
        if (world_.units[index].pursuit_target != target_id) {
          world_.units[index].pursuit_target = target_id;
          world_.units[index].pursuit_ticks = 0;
          world_.units[index].pursuit_anchor_x = unit.x;
          world_.units[index].pursuit_anchor_y = unit.y;
          if (events_ != nullptr) {
            events_->record(world_.tick_count + 1,
                            EventType::PursuitStart, unit.id);
          }
        }
        if (world_.units[index].pursuit_ticks <
            effective_tactics_->pursuit.max_ticks) {
          world_.units[index].pursuit_ticks += 1;
        }
      }
      // Tactic goals for this tick's movement: the search contact, the
      // ally in need, or the flank point (perpendicular offset from the
      // squad centroid off the squad-to-enemy axis; the member index
      // fixes the side deterministically).
      if (chosen == Action::Search && has_fresh_contact) {
        tactic_goal_x_[index] = search_goal_x;
        tactic_goal_y_[index] = search_goal_y;
        tactic_goal_set_[index] = 1;
      } else if (chosen == Action::Help && ally_in_need) {
        tactic_goal_x_[index] = world_.units[ally_goal].x;
        tactic_goal_y_[index] = world_.units[ally_goal].y;
        tactic_goal_set_[index] = 1;
      } else if (chosen == Action::Flank && enemies > 0 && has_squad) {
        const Squad& squad = squads_[static_cast<std::size_t>(
            unit_squad_[index])];
        double axis_x = perception.enemy_centroid_x - squad.centroid_x;
        double axis_y = perception.enemy_centroid_y - squad.centroid_y;
        const double length = std::sqrt(axis_x * axis_x + axis_y * axis_y);
        if (length > 1e-9) {
          axis_x /= length;
          axis_y /= length;
        } else {
          axis_x = unit.faction == Faction::Red ? 1.0 : -1.0;
          axis_y = 0.0;
        }
        const double side =
            (unit_member_[index] % 2 == 0) ? 1.0 : -1.0;
        const double offset = effective_tactics_->flank.offset_distance;
        tactic_goal_x_[index] =
            squad.centroid_x + (-axis_y * side) * offset;
        tactic_goal_y_[index] =
            squad.centroid_y + (axis_x * side) * offset;
        tactic_goal_set_[index] = 1;
      }
    }
  }

  // Debug capture: the explicitly requested tick, or the first tick on which
  // this unit sees an enemy (tick numbers are 1-based to match the report).
  if (ai_debug_.unit_id == unit.id && ai_debug_.unit_id != 0 &&
      !ai_debug_.captured) {
    const std::uint64_t tick_number = world_.tick_count + 1;
    const bool matches =
        ai_debug_.explicit_tick
            ? tick_number == ai_debug_.tick
            : perception.visible_enemies > 0;
    if (matches) {
      ai_debug_.captured = true;
      ai_debug_.captured_tick = tick_number;
      ai_debug_.health = unit.health;
      ai_debug_.max_health = unit.stats.max_health;
      ai_debug_.visible_enemies = perception.visible_enemies;
      ai_debug_.visible_allies = perception.visible_allies;
      ai_debug_.target = best;
      ai_debug_.target_distance = perception.nearest_visible_enemy_distance;
      ai_debug_.personality = unit.personality;
      ai_debug_.scores = scores;
      ai_debug_.chosen = chosen;
      ai_debug_.morale = unit.morale;
      ai_debug_.morale_state = unit.morale_state;
      ai_debug_.routed = unit.routed;
      // Order term (M6): the member's effective order on this tick, its
      // obedience weight and the per-action bonus that was applied.
      std::uint8_t order = 4;  // Order::None
      if (squads_active_ && unit_squad_[index] >= 0) {
        const Squad& squad = squads_[unit_squad_[index]];
        order = static_cast<std::uint8_t>(
            squad.members[unit_member_[index]].order);
      }
      ai_debug_.order = order;
      ai_debug_.obedience = obedience;
      ai_debug_.order_bonus = order_bonus;
      ai_debug_.pursuit_ticks = world_.units[index].pursuit_ticks;
      ai_debug_.has_fresh_contact = has_fresh_contact;
      ai_debug_.ally_in_need = ally_in_need;
      ai_debug_.member_may_flank = member_may_flank;
    }
  }
  return chosen;
}

// Phase 2 — movement. Destinations are computed from the tick-start
// snapshot first and committed afterwards, so a unit's move never depends on
// whether its target already moved this tick.
//
// Legacy mode: every unit with a target moves toward it (M1 rule).
// Utility mode: the chosen action drives the move — Approach toward the
// nearest visible enemy, Advance toward the faction objective, Retreat away
// from the visible enemy centroid and toward the unit's own side (clamped to
// the battlefield), Attack and Hold stay in place.
void Simulation::resolve_movement() {
  pending_moves_.assign(world_.units.size(), PendingMove{});

  if (config_.ai_mode == AiMode::Utility) {
    double advance_dx = 0.0;
    double advance_dy = 0.0;
    for (std::size_t i = 0; i < world_.units.size(); ++i) {
      const Unit& unit = world_.units[i];
      if (!unit.alive) {
        continue;
      }
      const Action action = static_cast<Action>(unit.last_action);
      const double speed = unit.stats.speed;
      // Stuck recovery (M8): a goal-seeking unit that made less than
      // kStuckEpsilon progress for kStuckTickThreshold consecutive ticks
      // follows its faction's flow field for kFlowFollowTicks ticks.
      // m11b: a squad with an active point order follows its own field.
      if (terrain_ != nullptr &&
          world_.tick_count < flow_follow_until_[i] &&
          order_flow_step(i, faction_index(unit.faction), unit.x, unit.y,
                          advance_dx, advance_dy)) {
        PendingMove move;
        move.moves = true;
        move.goal_seeking = true;
        move.x = unit.x + advance_dx * speed;
        move.y = unit.y + advance_dy * speed;
        pending_moves_[i] = move;
        continue;
      }
      // Formation steering (M6): squad members (never the acting commander,
      // who anchors the formation and moves by the normal rules) steer to
      // their slot whenever their action is Hold or Advance. Any other
      // action drops them out of formation (counted once per drop).
      if (squads_active_ && unit_squad_[i] >= 0) {
        Squad& squad = squads_[unit_squad_[i]];
        SquadMemberState& member = squad.members[unit_member_[i]];
        const bool is_commander = squad.commander_index == i;
        if (!is_commander &&
            (action == Action::Hold || action == Action::Advance)) {
          member.in_formation = true;
          if (config_.nav_v2 && terrain_ != nullptr &&
              action == Action::Advance) {
            // m11a navigation v2 — field-march: a member whose projected
            // slot is far away marches with the army along the faction
            // flow field instead of beelining to the slot. In a terrain
            // corridor (coastal strip, valley, ford line) the projected
            // slots bunch onto the few passable cells and the beeline
            // jams the whole squad against the water or the valley wall
            // (measured: swapped Lebanon parked 1040 units in one
            // 21-unit strip at a mean 0.036 units/tick). Following the
            // field keeps the squad moving at terrain speed; once the
            // slot is within kSlotMarchDist the normal shape steering
            // takes over again and the formation re-forms on open
            // ground. The march is goal-seeking, so the stuck
            // accounting sees members too.
            double slot_x = 0.0;
            double slot_y = 0.0;
            formation_slot_world(squad, static_cast<std::size_t>(
                                             unit_member_[i]),
                                 slot_x, slot_y);
            const double sx = slot_x - unit.x;
            const double sy = slot_y - unit.y;
            if (sx * sx + sy * sy >
                    kSlotMarchDist * kSlotMarchDist &&
                order_flow_step(i, faction_index(unit.faction), unit.x,
                                unit.y, advance_dx, advance_dy)) {
              PendingMove move;
              move.moves = true;
              move.goal_seeking = true;
              move.x = unit.x + advance_dx * speed;
              move.y = unit.y + advance_dy * speed;
              pending_moves_[i] = move;
              continue;
            }
          }
          if (steer_in_formation(i, unit, squad, member)) {
            continue;
          }
        } else if (member.in_formation) {
          member.in_formation = false;
          squad_stats_[faction_index(unit.faction)].drops_from_formation += 1;
        }
      }
      if (action == Action::Search || action == Action::Help ||
          action == Action::Flank) {
        // M10 tactic moves: straight toward this tick's stored goal (the
        // remembered/reported contact, the ally in need, or the flank
        // point). No stored goal (edge cases): treat like Hold.
        if (tactic_goal_set_[i]) {
          const double dx = tactic_goal_x_[i] - unit.x;
          const double dy = tactic_goal_y_[i] - unit.y;
          const double dist_sq = dx * dx + dy * dy;
          if (dist_sq > 1e-12) {
            const double dist = std::sqrt(dist_sq);
            const double step = std::min(speed, dist);
            PendingMove move;
            move.moves = true;
            move.goal_seeking = true;
            move.x = unit.x + dx / dist * step;
            move.y = unit.y + dy / dist * step;
            pending_moves_[i] = move;
          }
        }
      } else if (action == Action::Approach) {
        const int target = target_index_[i];
        if (target < 0) {
          continue;  // defensive: Approach implies a visible enemy
        }
        const Unit& goal = world_.units[static_cast<std::size_t>(target)];
        const double dx = goal.x - unit.x;
        const double dy = goal.y - unit.y;
        const double dist_sq = dx * dx + dy * dy;
        if (dist_sq <= 0.0) {
          continue;
        }
        const double dist = std::sqrt(dist_sq);
        const double step = std::min(speed, dist);
        PendingMove move;
        move.moves = true;
        move.goal_seeking = true;
        move.x = unit.x + dx / dist * step;
        move.y = unit.y + dy / dist * step;
        pending_moves_[i] = move;
      } else if (action == Action::Advance && terrain_ != nullptr &&
                 order_flow_step(i, faction_index(unit.faction), unit.x,
                                 unit.y, advance_dx, advance_dy)) {
        // M8: Advance follows the faction's cached flow field around
        // impassable terrain instead of the straight line (m11b: a
        // squad with an active point order follows its own field).
        PendingMove move;
        move.moves = true;
        move.goal_seeking = true;
        move.x = unit.x + advance_dx * speed;
        move.y = unit.y + advance_dy * speed;
        pending_moves_[i] = move;
      } else if (action == Action::Advance) {
        const std::size_t faction = faction_index(unit.faction);
        // m12 audit: a delivered point order steers Advance on flat maps
        // too (the squad flow field only exists with terrain; before, the
        // point was accepted, recorded — and ignored without terrain).
        // Within kOrderArrivalDist of the point the unit holds position.
        double goal_x = objective_x_[faction];
        double goal_y = objective_y_[faction];
        bool to_point = false;
        if (squads_active_ && unit_squad_[i] >= 0) {
          const Squad& squad =
              squads_[static_cast<std::size_t>(unit_squad_[i])];
          if (squad.player_order_active && squad.player_order_point) {
            goal_x = squad.player_order_x;
            goal_y = squad.player_order_y;
            to_point = true;
          }
        }
        const double dx = goal_x - unit.x;
        const double dy = goal_y - unit.y;
        const double dist_sq = dx * dx + dy * dy;
        const double stop_dist_sq =
            to_point ? kOrderArrivalDist * kOrderArrivalDist : 0.0;
        if (dist_sq <= stop_dist_sq) {
          continue;  // already at the objective / ordered point
        }
        const double dist = std::sqrt(dist_sq);
        const double step = std::min(speed, dist);
        PendingMove move;
        move.moves = true;
        move.goal_seeking = true;
        move.x = unit.x + dx / dist * step;
        move.y = unit.y + dy / dist * step;
        pending_moves_[i] = move;
      } else if (action == Action::Retreat) {
        // m12 audit: on flat maps a point retreat withdraws straight to
        // the point at half speed (the flow-field path needs terrain);
        // at the point the withdrawal is complete and the unit holds.
        if (terrain_ == nullptr && squads_active_ && unit_squad_[i] >= 0) {
          const Squad& squad =
              squads_[static_cast<std::size_t>(unit_squad_[i])];
          if (squad.player_order_active && squad.player_order_point) {
            const double dx = squad.player_order_x - unit.x;
            const double dy = squad.player_order_y - unit.y;
            const double dist_sq = dx * dx + dy * dy;
            if (dist_sq > kOrderArrivalDist * kOrderArrivalDist) {
              const double dist = std::sqrt(dist_sq);
              const double step = std::min(0.5 * speed, dist);
              PendingMove move;
              move.moves = true;
              move.goal_seeking = true;
              move.x = unit.x + dx / dist * step;
              move.y = unit.y + dy / dist * step;
              pending_moves_[i] = move;
            }
            continue;
          }
        }
        // m11b: a squad with an active point order withdraws to that
        // point (fighting withdrawal at half speed along the squad's
        // field) instead of straight away from the enemy.
        if (terrain_ != nullptr && squads_active_ && unit_squad_[i] >= 0) {
          const Squad& squad = squads_[static_cast<std::size_t>(
              unit_squad_[i])];
          if (squad.player_order_point &&
              order_flow_step(i, faction_index(unit.faction), unit.x,
                              unit.y, advance_dx, advance_dy)) {
            const double step = 0.5 * speed;
            PendingMove move;
            move.moves = true;
            move.goal_seeking = true;
            move.x = unit.x + advance_dx * step;
            move.y = unit.y + advance_dy * step;
            pending_moves_[i] = move;
            continue;
          }
        }
        const Perception& perception = last_perception_[i];
        if (perception.visible_enemies == 0) {
          continue;  // defensive: Retreat implies visible enemies
        }
        double dx = unit.x - perception.enemy_centroid_x;
        double dy = unit.y - perception.enemy_centroid_y;
        // Force the horizontal component toward the unit's own side (Red
        // holds the left band, Blue the right band).
        if (unit.faction == Faction::Red) {
          dx = -std::fabs(dx);
        } else {
          dx = std::fabs(dx);
        }
        double length = std::sqrt(dx * dx + dy * dy);
        if (!(length > 0.0)) {
          // Surrounded exactly at the centroid: fall back to straight toward
          // the unit's own side.
          dx = unit.faction == Faction::Red ? -1.0 : 1.0;
          dy = 0.0;
          length = 1.0;
        }
        // A fighting withdrawal at half speed: a full-speed retreat would
        // kite equal-speed pursuers forever and stall battles.
        const double step = 0.5 * speed;
        PendingMove move;
        move.moves = true;
        move.x = std::clamp(unit.x + dx / length * step, 0.0, world_.width);
        move.y = std::clamp(unit.y + dy / length * step, 0.0, world_.height);
        pending_moves_[i] = move;
      } else if (action == Action::Flee) {
        // Routed units (M5) flee at full speed: away from the visible enemy
        // centroid and toward their own side; with no enemies visible, straight
        // toward their own side. A unit pressed against both relevant
        // boundaries is cornered and flees toward the nearest world edge.
        double dx;
        double dy;
        const Perception& perception = last_perception_[i];
        if (perception.visible_enemies > 0) {
          dx = unit.x - perception.enemy_centroid_x;
          dy = unit.y - perception.enemy_centroid_y;
          if (unit.faction == Faction::Red) {
            dx = -std::fabs(dx);
          } else {
            dx = std::fabs(dx);
          }
        } else {
          dx = unit.faction == Faction::Red ? -1.0 : 1.0;
          dy = 0.0;
        }
        double length = std::sqrt(dx * dx + dy * dy);
        if (!(length > 0.0)) {
          dx = unit.faction == Faction::Red ? -1.0 : 1.0;
          dy = 0.0;
          length = 1.0;
        }
        const double raw_x = unit.x + dx / length * speed;
        const double raw_y = unit.y + dy / length * speed;
        const bool x_clamped = raw_x < 0.0 || raw_x > world_.width;
        const bool y_clamped = raw_y < 0.0 || raw_y > world_.height;
        double dest_x = std::clamp(raw_x, 0.0, world_.width);
        double dest_y = std::clamp(raw_y, 0.0, world_.height);
        if (x_clamped && y_clamped) {
          // Cornered: toward the nearest of the four world edges.
          const double to_left = unit.x;
          const double to_bottom = unit.y;
          const double to_right = world_.width - unit.x;
          const double to_top = world_.height - unit.y;
          if (to_left <= to_right && to_left <= to_bottom &&
              to_left <= to_top) {
            dest_x = std::clamp(unit.x - speed, 0.0, world_.width);
            dest_y = unit.y;
          } else if (to_right <= to_left && to_right <= to_bottom &&
                     to_right <= to_top) {
            dest_x = std::clamp(unit.x + speed, 0.0, world_.width);
            dest_y = unit.y;
          } else if (to_bottom <= to_top) {
            dest_x = unit.x;
            dest_y = std::clamp(unit.y - speed, 0.0, world_.height);
          } else {
            dest_x = unit.x;
            dest_y = std::clamp(unit.y + speed, 0.0, world_.height);
          }
        }
        PendingMove move;
        move.moves = true;
        move.x = dest_x;
        move.y = dest_y;
        pending_moves_[i] = move;
      }
      // Attack, Hold (and the pre-battle None) do not move.
    }
  } else {
    for (std::size_t i = 0; i < world_.units.size(); ++i) {
      const Unit& unit = world_.units[i];
      const int target = target_index_[i];
      if (!unit.alive || target < 0) {
        continue;
      }
      const Unit& goal = world_.units[static_cast<std::size_t>(target)];
      const double dx = goal.x - unit.x;
      const double dy = goal.y - unit.y;
      const double dist_sq = dx * dx + dy * dy;
      if (dist_sq <= 0.0) {
        continue;  // already at the target's position
      }
      const double dist = std::sqrt(dist_sq);
      const double step = std::min(unit.stats.speed, dist);
      PendingMove move;
      move.moves = true;
      move.x = unit.x + dx / dist * step;
      move.y = unit.y + dy / dist * step;
      pending_moves_[i] = move;
    }
  }

  for (std::size_t i = 0; i < world_.units.size(); ++i) {
    Unit& unit = world_.units[i];
    if (combat_v2_ &&
        unit.fatigue >= effective_combat_->fatigue.exhausted_level) {
      // m13: an exhausted unit moves at the fatigue speed factor (the
      // displacement scales uniformly, which is the speed scaling for
      // this tick's move).
      pending_moves_[i].x =
          unit.x + (pending_moves_[i].x - unit.x) *
                       effective_combat_->fatigue.speed_factor;
      pending_moves_[i].y =
          unit.y + (pending_moves_[i].y - unit.y) *
                       effective_combat_->fatigue.speed_factor;
    }
    if (!pending_moves_[i].moves) {
      // A unit that stands still has no heading: reset so the squad's
      // alignment average never reads a stale move.
      unit.last_move_x = 0.0;
      unit.last_move_y = 0.0;
      if (terrain_ != nullptr) {
        stuck_ticks_[i] = 0;  // not trying to move: not stuck
      }
      continue;
    }
    if (terrain_ != nullptr) {
      // M8: terrain commit (move_cost scaling, wall sliding, the
      // impassable invariant and stuck-progress bookkeeping).
      commit_move_terrain(i, pending_moves_[i]);
      continue;
    }
    // Record the committed displacement (M6 alignment steering reads it).
    unit.last_move_x = pending_moves_[i].x - unit.x;
    unit.last_move_y = pending_moves_[i].y - unit.y;
    unit.x = pending_moves_[i].x;
    unit.y = pending_moves_[i].y;
  }
  if (combat_v2_) {
    // m13: the heading follows the committed displacement (both commit
    // paths set last_move; a standing unit keeps its heading).
    for (Unit& unit : world_.units) {
      if (unit.alive &&
          (unit.last_move_x != 0.0 || unit.last_move_y != 0.0)) {
        unit.facing = std::atan2(unit.last_move_y, unit.last_move_x);
      }
    }
  }
  update_grid_after_movement();
}

// Formation steering (M6) for one member in formation. The steering vector
// is the weighted sum of slot seeking, cohesion (toward the squad's last
// computed centroid), alignment (the squad's mean last move) and separation
// (away from every unit inside separation_radius, any squad, any faction);
// the member then moves min(speed, |steering|) along it, clamped to the
// battlefield. All reads come from the tick-start snapshot, exactly like
// every other move. Always returns true — steering owns the member's
// movement for this tick (a zero vector means "stay", which is the correct
// move for a member already on its slot).
bool Simulation::steer_in_formation(std::size_t index, const Unit& unit,
                                    const Squad& squad,
                                    const SquadMemberState& /*member*/) {
  const std::size_t faction = faction_index(unit.faction);
  // Anchor: the commander's position while commanded; a leaderless squad
  // keeps its shape around its last computed centroid.
  double slot_x = 0.0;
  double slot_y = 0.0;
  formation_slot_world(squad, unit_member_[index], slot_x, slot_y);

  const FormationConfig::Steering& steering = formations_->steering;
  double vx = steering.slot_seeking * (slot_x - unit.x);
  double vy = steering.slot_seeking * (slot_y - unit.y);
  vx += steering.cohesion * (squad.centroid_x - unit.x);
  vy += steering.cohesion * (squad.centroid_y - unit.y);
  // Alignment: the squad's mean last move (units that have not moved yet
  // contribute nothing, so a stationary squad has no alignment pull).
  double move_x = 0.0;
  double move_y = 0.0;
  std::size_t moved = 0;
  for (const SquadMemberState& other : squad.members) {
    const Unit& other_unit = world_.units[other.unit_index];
    if (!other_unit.alive) {
      continue;
    }
    if (other_unit.last_move_x != 0.0 || other_unit.last_move_y != 0.0) {
      move_x += other_unit.last_move_x;
      move_y += other_unit.last_move_y;
      ++moved;
    }
  }
  if (moved > 0) {
    vx += steering.alignment * (move_x / static_cast<double>(moved));
    vy += steering.alignment * (move_y / static_cast<double>(moved));
  }
  // Separation: pushed away from every unit strictly inside
  // separation_radius, linearly stronger the closer it is. Exact overlaps
  // (distance 0) have no defined direction and are skipped; other neighbors
  // still push the pair apart.
  grid_.radius_query(world_.units, unit.x, unit.y, steering.separation_radius,
                     separation_buffer_);
  for (std::uint32_t other_index : separation_buffer_) {
    if (other_index == index) {
      continue;
    }
    const Unit& other = world_.units[other_index];
    const double dx = unit.x - other.x;
    const double dy = unit.y - other.y;
    const double distance = std::sqrt(dx * dx + dy * dy);
    if (!(distance < steering.separation_radius)) {
      continue;  // at or beyond the minimum distance: no push
    }
    if (distance <= 0.0) {
      continue;
    }
    const double weight =
        (steering.separation_radius - distance) / steering.separation_radius;
    vx += steering.separation * weight * dx / distance;
    vy += steering.separation * weight * dy / distance;
  }
  const double magnitude = std::sqrt(vx * vx + vy * vy);
  if (magnitude > 0.0) {
    const double step = std::min(unit.stats.speed, magnitude);
    PendingMove move;
    move.moves = true;
    move.x = std::clamp(unit.x + vx / magnitude * step, 0.0, world_.width);
    move.y = std::clamp(unit.y + vy / magnitude * step, 0.0, world_.height);
    pending_moves_[index] = move;
  }
  return true;
}

// Phase 0 continuation — spatial index maintenance. Re-buckets every unit
// that moved this tick, in fixed unit-index order.
// M10: publishes this tick's perceptions into the squad contact lists
// (called at the end of decide_targets). Members inside the acting
// commander's leadership radius share the position of the nearest enemy
// they see; the list keeps the freshest kMaxSquadContacts entries,
// newest first. Readers honor squad_intel_delay_ticks in
// decide_unit_action, so intel — like orders — travels with a delay.
void Simulation::update_squad_contacts() {
  if (!tactics_active_ || !squads_active_) {
    return;
  }
  const std::uint64_t tick_number = world_.tick_count + 1;
  for (Squad& squad : squads_) {
    if (!squad.command_active) {
      continue;
    }
    const Unit& commander = world_.units[squad.commander_index];
    const double radius = squad.leadership_radius;
    for (const SquadMemberState& member : squad.members) {
      const Perception& perception = last_perception_[member.unit_index];
      if (perception.visible_enemies == 0 ||
          perception.nearest_visible_enemy < 0) {
        continue;
      }
      const Unit& seer = world_.units[member.unit_index];
      const double dx = seer.x - commander.x;
      const double dy = seer.y - commander.y;
      if (dx * dx + dy * dy > radius * radius) {
        continue;  // out of command reach: no sharing
      }
      const Unit& enemy =
          world_.units[static_cast<std::size_t>(
              perception.nearest_visible_enemy)];
      SquadContact contact;
      contact.x = enemy.x;
      contact.y = enemy.y;
      contact.tick = tick_number;
      // Insert newest-first, dropping the oldest past the cap.
      const std::size_t kept = squad.contact_count < kMaxSquadContacts
                                   ? squad.contact_count
                                   : kMaxSquadContacts - 1;
      for (std::size_t c = kept; c > 0; --c) {
        squad.contacts[c] = squad.contacts[c - 1];
      }
      squad.contacts[0] = contact;
      squad.contact_count =
          static_cast<std::uint32_t>(std::min(kept + 1, kMaxSquadContacts));
    }
  }
}

// M10 continuation: refreshes the faction-level enemy estimate from this
// tick's perceptions — the mean position of the nearest visible enemy of
// every member that sees one (unit index order, so it is deterministic).
// The estimate is delayed by nature (it is the LAST sighting, updated
// only while contact exists) and noisy (only seen enemies contribute),
// which is exactly the coarse intelligence a parked army needs to start
// seeking again.
void Simulation::update_faction_estimates() {
  if (!tactics_active_ || !contact_seek_active_) {
    return;
  }
  const std::uint64_t tick_number = world_.tick_count + 1;
  for (std::size_t faction = 0; faction < kFactionCount; ++faction) {
    double sum_x = 0.0;
    double sum_y = 0.0;
    std::size_t seers = 0;
    for (std::size_t i = 0; i < world_.units.size(); ++i) {
      const Unit& unit = world_.units[i];
      if (!unit.alive || faction_index(unit.faction) != faction) {
        continue;
      }
      const Perception& perception = last_perception_[i];
      if (perception.visible_enemies == 0 ||
          perception.nearest_visible_enemy < 0) {
        continue;
      }
      const Unit& enemy = world_.units[static_cast<std::size_t>(
          perception.nearest_visible_enemy)];
      sum_x += enemy.x;
      sum_y += enemy.y;
      ++seers;
    }
    if (seers > 0) {
      estimate_x_[faction] = sum_x / static_cast<double>(seers);
      estimate_y_[faction] = sum_y / static_cast<double>(seers);
      estimate_tick_[faction] = tick_number;
    }
  }
}

void Simulation::update_grid_after_movement() {
  if (!use_grid_) {
    return;
  }
  for (std::size_t i = 0; i < world_.units.size(); ++i) {
    if (pending_moves_[i].moves) {
      grid_.update(world_.units, static_cast<std::uint32_t>(i));
    }
  }
}

// m13 item 3 (--combat v2): commander auras and the defensive stance,
// computed once per tick at combat time (positions are final after
// movement). The aura covers same-faction units inside a commander's
// (leadership_radius > 0) radius; the stance marks units whose chosen
// action is Hold and whose squad's cohesion is above
// stance.cohesion_min.
void Simulation::update_command_auras() {
  std::fill(aura_active_.begin(), aura_active_.end(), 0);
  std::fill(defensive_stance_.begin(), defensive_stance_.end(), 0);
  for (std::size_t i = 0; i < world_.units.size(); ++i) {
    const Unit& unit = world_.units[i];
    if (!unit.alive || unit.stats.leadership_radius <= 0.0) {
      continue;
    }
    grid_.radius_query(world_.units, unit.x, unit.y,
                       unit.stats.leadership_radius,
                       combat_probe_buffer_);
    for (std::uint32_t probe : combat_probe_buffer_) {
      if (world_.units[probe].alive &&
          world_.units[probe].faction == unit.faction) {
        aura_active_[probe] = 1;
      }
    }
  }
  if (squads_active_) {
    // The stance needs a squad (its cohesion); without squads the
    // per-unit squad index and squad table do not exist.
    for (std::size_t i = 0; i < world_.units.size(); ++i) {
      const Unit& unit = world_.units[i];
      if (!unit.alive ||
          static_cast<Action>(unit.last_action) != Action::Hold) {
        continue;
      }
      if (unit_squad_[i] < 0) {
        continue;
      }
      const Squad& squad =
          squads_[static_cast<std::size_t>(unit_squad_[i])];
      if (squad.cohesion >= effective_combat_->stance.cohesion_min) {
        defensive_stance_[i] = 1;
      }
    }
  }
}

// m13 item 3: per-tick fatigue and the charge run ring, after combat
// (both this tick's moves and attacks are known). Fight fatigue is
// charged per attack in resolve_combat; this pass handles full-speed
// moves, idle recovery (a fighting unit neither gains nor recovers
// here) and records the displacement the charge check projects.
void Simulation::update_fatigue_experience() {
  for (std::size_t i = 0; i < world_.units.size(); ++i) {
    const Unit& unit = world_.units[i];
    if (!unit.alive) {
      continue;
    }
    const double moved_sq = unit.last_move_x * unit.last_move_x +
                            unit.last_move_y * unit.last_move_y;
    const double cruise = 0.9 * unit.stats.speed;
    const bool full_speed = moved_sq >= cruise * cruise;
    const bool attacked = attacked_this_tick_[i] != 0;
    const bool idle = moved_sq == 0.0 && !attacked;
    if (full_speed) {
      world_.units[i].fatigue =
          std::clamp(unit.fatigue +
                         effective_combat_->fatigue.move_gain_per_tick,
                     0.0, 1.0);
    } else if (idle) {
      world_.units[i].fatigue =
          std::clamp(unit.fatigue -
                         effective_combat_->fatigue.recover_per_tick,
                     0.0, 1.0);
    }
    const std::size_t base = i * 2 * charge_window_;
    const std::size_t slot = base + 2 * charge_ring_pos_[i];
    charge_ring_[slot] = unit.last_move_x;
    charge_ring_[slot + 1] = unit.last_move_y;
    charge_ring_pos_[i] = (charge_ring_pos_[i] + 1) % charge_window_;
  }
}

// Phase 3 — combat. First every attack is decided from the post-movement
// state (alive, in range, cooldown ready), then all attacks resolve
// simultaneously: two units can kill each other in the same tick and focus
// fire can overshoot, which is clamped in apply_deaths(). Legacy mode
// attacks with every unit in range (M1 rule); utility mode attacks only
// with units whose chosen action is Attack.
void Simulation::resolve_combat() {
  pending_attacks_.clear();
  if (combat_v2_) {
    update_command_auras();
  }
  const bool utility = config_.ai_mode == AiMode::Utility;
  // M10: combat rolls are keyed by (seed, attacker id, tick, channel), so
  // the resolve order can never matter. With tactics off no roll exists
  // and combat is exactly the M1-M9 deterministic rule.
  const std::uint64_t tick_number = world_.tick_count + 1;

  for (std::size_t i = 0; i < world_.units.size(); ++i) {
    const Unit& unit = world_.units[i];
    const int target = target_index_[i];
    if (!unit.alive || target < 0 || unit.cooldown_remaining != 0) {
      continue;
    }
    if (utility &&
        static_cast<Action>(unit.last_action) != Action::Attack) {
      continue;  // only the Attack action engages
    }
    const Unit& goal = world_.units[static_cast<std::size_t>(target)];
    if (!goal.alive) {
      continue;  // defensive: targets selected this tick are alive
    }
    const double dx = goal.x - unit.x;
    const double dy = goal.y - unit.y;
    const double range = unit.stats.attack_range;
    if (dx * dx + dy * dy > range * range) {
      continue;  // out of range (boundary is inclusive)
    }
    PendingAttack attack;
    attack.attacker = i;
    attack.target = static_cast<std::size_t>(target);
    attack.damage = unit.stats.damage;
    if (combat_v2_) {
      // m13: experience sharpens accuracy, exhaustion dulls it.
      double accuracy = unit.stats.accuracy +
                        effective_combat_->experience.accuracy_bonus_max *
                            unit.experience;
      if (unit.fatigue >= effective_combat_->fatigue.exhausted_level) {
        accuracy *= effective_combat_->fatigue.accuracy_factor;
      }
      attack.accuracy = std::clamp(accuracy, 0.0, 1.0);
    } else {
      attack.accuracy = unit.stats.accuracy;
    }
    if (tactics_active_) {
      // Accuracy: a miss still costs the attack (cooldown) but connects
      // with nothing.
      const double hit_roll =
          tactics_roll(config_.seed, unit.id, tick_number, 0);
      if (hit_roll >= attack.accuracy) {
        attack.damage = 0.0;
      } else if (unit.stats.damage_variance > 0.0) {
        const double roll =
            tactics_roll(config_.seed, unit.id, tick_number, 1);
        attack.damage *=
            1.0 + unit.stats.damage_variance * (2.0 * roll - 1.0);
      }
    }
    if (combat_v2_) {
      // m13 item 3: the attacker-side depth multipliers.
      double multiplier = 1.0;
      // Charge: a fast unit (type speed >= charge.min_speed) that
      // covered at least min_distance toward this target over the run
      // window.
      if (unit.stats.speed >= effective_combat_->charge.min_speed) {
        const double tdx = goal.x - unit.x;
        const double tdy = goal.y - unit.y;
        const double tlen = std::sqrt(tdx * tdx + tdy * tdy);
        // The direction to project the run on: toward the target — or,
        // for a coincident pair (approach ends on the target's spot),
        // the direction of the summed run itself.
        double ux = 0.0;
        double uy = 0.0;
        const std::size_t base = i * 2 * charge_window_;
        if (tlen > 0.0) {
          ux = tdx / tlen;
          uy = tdy / tlen;
        } else {
          double sx = 0.0;
          double sy = 0.0;
          for (std::size_t k = 0; k < charge_window_; ++k) {
            sx += charge_ring_[base + 2 * k];
            sy += charge_ring_[base + 2 * k + 1];
          }
          const double slen = std::sqrt(sx * sx + sy * sy);
          if (slen <= 0.0) {
            ux = -1.0;  // no run at all: the projection below is 0
          } else {
            ux = sx / slen;
            uy = sy / slen;
          }
        }
        if (ux != 0.0 || uy != 0.0) {
          double run = 0.0;
          for (std::size_t k = 0; k < charge_window_; ++k) {
            run += charge_ring_[base + 2 * k] * ux +
                   charge_ring_[base + 2 * k + 1] * uy;
          }
          if (run >= effective_combat_->charge.min_distance) {
            bool reduced =
                defensive_stance_[static_cast<std::size_t>(target)] !=
                    0 ||
                static_cast<Action>(goal.last_action) == Action::Hold;
            if (!reduced) {
              // A dense line around the target blunts the charge too.
              grid_.radius_query(
                  world_.units, goal.x, goal.y,
                  effective_combat_->charge.dense_line_radius,
                  combat_probe_buffer_);
              std::uint32_t allies = 0;
              for (std::uint32_t probe : combat_probe_buffer_) {
                const Unit& other = world_.units[probe];
                if (other.alive && other.faction == goal.faction &&
                    probe != static_cast<std::uint32_t>(target)) {
                  ++allies;
                }
              }
              reduced =
                  allies >= effective_combat_->charge.dense_line_allies;
            }
            double charge_mult = effective_combat_->charge.multiplier;
            if (reduced) {
              charge_mult =
                  1.0 + (charge_mult - 1.0) *
                            effective_combat_->charge.held_reduction;
            }
            multiplier *= charge_mult;
            attack.charge_hit = true;
          }
        }
      }
      // Facing: the arc at the target (front / flank / rear).
      const AttackArc arc =
          attack_arc(goal.facing, unit.x - goal.x, unit.y - goal.y,
                     *effective_combat_);
      multiplier *= arc.multiplier;
      attack.flank_hit = arc.flank;
      attack.rear_hit = arc.rear;
      // Fatigue and the commander's aura.
      if (unit.fatigue >= effective_combat_->fatigue.exhausted_level) {
        multiplier *= effective_combat_->fatigue.damage_factor;
      }
      if (aura_active_[i] != 0) {
        multiplier *= 1.0 + effective_combat_->aura.damage_bonus;
      }
      attack.damage *= multiplier;
      // The defender's high-cohesion Hold stance.
      if (defensive_stance_[static_cast<std::size_t>(target)] != 0) {
        attack.damage *= effective_combat_->stance.damage_factor;
      }
    }
    if (terrain_ != nullptr) {
      // M8: damage taken scales with the target's terrain (hills 0.75,
      // forest 0.85, ...).
      attack.damage *= terrain_->defense_factor_at(goal.x, goal.y);
    }
    if (attack.damage < 0.0) {
      attack.damage = 0.0;  // defensive: variance is capped at 100%
    }
    pending_attacks_.push_back(attack);
  }

  for (const PendingAttack& attack : pending_attacks_) {
    Unit& attacker = world_.units[attack.attacker];
    Unit& goal = world_.units[attack.target];
    const double health_before = goal.health;
    goal.health -= attack.damage;
    if (tactics_active_ && attacker.stats.cooldown_jitter > 0.0) {
      // Cooldown jitter (M10): spread the recovery by +/- the jitter
      // fraction of the base cooldown, at least 1 tick.
      const double roll =
          tactics_roll(config_.seed, attacker.id, tick_number, 2);
      const double factor =
          1.0 + attacker.stats.cooldown_jitter * (2.0 * roll - 1.0);
      const double spread = static_cast<double>(
                                 attacker.stats.attack_cooldown_ticks) *
                            factor;
      attacker.cooldown_remaining = static_cast<std::uint32_t>(
          std::max(1.0, std::floor(spread + 0.5)));
    } else {
      attacker.cooldown_remaining = attacker.stats.attack_cooldown_ticks;
    }
    // Morale bookkeeping (M5): damage taken and kills this tick feed
    // update_morale. Accumulated unconditionally (two array writes) and
    // reset there every tick; with morale off nothing reads them.
    damage_this_tick_[attack.target] += attack.damage;
    if (combat_v2_) {
      // m13 item 3: fight fatigue and engagement experience per attack
      // (a swing costs, hit or miss), and the report counters.
      attacked_this_tick_[attack.attacker] = 1;
      attacker.fatigue =
          std::clamp(attacker.fatigue +
                         effective_combat_->fatigue.fight_gain_per_attack,
                     0.0, 1.0);
      attacker.experience =
          std::clamp(attacker.experience +
                         effective_combat_->experience
                             .gain_per_engaged_tick,
                     0.0, 1.0);
      if (attack.damage > 0.0) {
        CombatStats& stats =
            faction_reports_[faction_index(attacker.faction)].combat;
        if (attack.flank_hit) {
          ++stats.flank_hits;
        }
        if (attack.rear_hit) {
          ++stats.rear_hits;
          ++rear_hits_this_tick_[attack.target];
        }
        if (attack.charge_hit) {
          ++stats.charge_hits;
        }
        if (defensive_stance_[attack.target] != 0) {
          ++faction_reports_[faction_index(goal.faction)]
               .combat.stance_hits_taken;
        }
      }
      if (attack.charge_hit) {
        // The run is spent: reset the attacker's ring.
        const std::size_t base = attack.attacker * 2 * charge_window_;
        std::fill(charge_ring_.begin() +
                      static_cast<std::ptrdiff_t>(base),
                  charge_ring_.begin() +
                      static_cast<std::ptrdiff_t>(
                          base + 2 * charge_window_),
                  0.0);
        charge_ring_pos_[attack.attacker] = 0;
      }
    }
    if (health_before > 0.0 && goal.health <= 0.0) {
      kills_this_tick_[attack.attacker] += 1;
      if (combat_v2_) {
        // m13: kills teach.
        attacker.experience = std::clamp(
            attacker.experience +
                effective_combat_->experience.gain_per_kill,
            0.0, 1.0);
      }
      if (events_ != nullptr) {
        // M10 Part 2a: the killing blow (the same condition that credits
        // the type kill above), with victim and killer ids.
        events_->record(tick_number, EventType::Death, goal.id, attacker.id);
      }
    }
    if (use_type_reports_) {
      // Credit the attacker's type: all applied damage counts (overkill
      // included, matching M1 semantics); the kill is credited once, to the
      // attack that brought the target from positive health to <= 0.
      TypeReport& report =
          type_reports_[faction_index(attacker.faction)][attacker.type_index];
      report.damage_dealt += attack.damage;
      if (health_before > 0.0 && goal.health <= 0.0) {
        report.kills += 1;
      }
    }
  }
  if (combat_v2_) {
    update_fatigue_experience();  // once per tick, after the resolves
  }
}

// Phase 4 — deaths. Health is clamped to zero so exported values stay clean.
// Dead units leave the spatial index immediately (grid mode), in fixed
// unit-index order. Each death is recorded as an event (M5) so nearby allies
// lose morale for it in update_morale. A commander's death (M6) also costs
// its squad the command: members inside the leadership radius take the
// morale shock immediately (personality-scaled like every other morale
// loss, so a routed-by-shock member breaks in update_morale this same
// tick), all member orders drop to None and the squad drifts until
// succession (update_squads).
void Simulation::apply_deaths() {
  for (std::size_t i = 0; i < world_.units.size(); ++i) {
    Unit& unit = world_.units[i];
    if (unit.alive && unit.health <= 0.0) {
      unit.alive = false;
      unit.health = 0.0;
      unit.cooldown_remaining = 0;
      faction_reports_[faction_index(unit.faction)].deaths += 1;
      if (terrain_ != nullptr) {
        const std::uint16_t cell = terrain_->type_index_at(unit.x, unit.y);
        if (cell != TerrainGrid::kNoCell) {
          terrain_kills_[cell] += 1;
        }
      }
      if (morale_enabled_) {
        death_events_.push_back(DeathEvent{unit.x, unit.y, unit.faction});
      }
      // Commander death (M6): only the acting commander's death costs the
      // squad its command (a deposed, already-succeeded or routed former
      // commander dying later is just a casualty).
      if (squads_active_ && unit_squad_[i] >= 0) {
        Squad& squad = squads_[unit_squad_[i]];
        bool any_alive = false;
        for (const SquadMemberState& member : squad.members) {
          if (world_.units[member.unit_index].alive) {
            any_alive = true;
            break;
          }
        }
        if (!any_alive) {
          if (events_ != nullptr) {
            events_->record(world_.tick_count + 1,
                            EventType::SquadCollapse, squad.id);
          }
          // m12 audit: docs/squads.md says a player order ends on squad
          // collapse; clear_player_order was dead code until now. (A
          // no-op for squads without orders: the fields are already
          // false/0 and the flow slot empty, so no-order runs stay
          // byte-identical.)
          clear_player_order(squad);
        }
        if (squad.commander_index == i && squad.command_active) {
          const std::size_t faction = faction_index(unit.faction);
          squad_stats_[faction].commander_deaths += 1;
          if (events_ != nullptr) {
            events_->record(world_.tick_count + 1,
                            EventType::CommanderDeath, unit.id, squad.id);
          }
          squad.command_active = false;
          squad.command_lost_tick = world_.tick_count + 1;  // 1-based
          // m11b: a delivered player order is the faction's order, not
          // the commander's — the successor inherits it (in-flight
          // orders deliver per their issue-time delay whatever the
          // command state). Only AI orders clear on command loss.
          if (!squad.player_order_active) {
            squad.order = Order::None;
            for (SquadMemberState& member : squad.members) {
              member.order = Order::None;  // drift with no order term
            }
          }
          const double radius_sq =
              squad.leadership_radius * squad.leadership_radius;
          for (SquadMemberState& member : squad.members) {
            Unit& member_unit = world_.units[member.unit_index];
            if (!member_unit.alive) {
              continue;
            }
            const double dx = member_unit.x - unit.x;
            const double dy = member_unit.y - unit.y;
            if (dx * dx + dy * dy > radius_sq) {
              continue;  // shock reaches members inside the command radius
            }
            const MoraleConfig& morale = *effective_morale_;
            const double scale = std::clamp(
                1.0 + morale.loss.caution_amplifier *
                          member_unit.personality.caution -
                    morale.loss.bravery_shield * member_unit.personality.bravery,
                morale.loss.min_scale, 2.0);
            member_unit.morale =
                std::clamp(member_unit.morale -
                               commanders_->commander_death_morale_loss * scale,
                           0.0, 100.0);
            member_unit.morale_state = static_cast<std::uint8_t>(
                state_from_morale(member_unit.morale, morale.states));
          }
        }
      }
      if (use_grid_) {
        grid_.remove(world_.units, static_cast<std::uint32_t>(i));
      }
    }
  }
}

// Phase 5 — morale, routing and recovery (M5). Runs after deaths so this
// tick's events are included; reads perception from the tick-start snapshot
// (the decide phase stored it in last_perception_). Deterministic: units in
// index order, death events in index order.
void Simulation::update_morale() {
  if (morale_enabled_) {
    const MoraleConfig& morale = *effective_morale_;

    // Shared helper: for each recorded event (death or rout), count it for
    // every same-faction observer within its own sight radius. One grid
    // query per event at the battle's largest sight radius; the observers
    // then apply their own, smaller-or-equal radius. Counts are
    // order-independent, so the grid traversal order cannot matter.
    const auto spread_events = [&](const std::vector<DeathEvent>& events,
                                   std::vector<std::uint32_t>& counts) {
      if (events.empty() || max_sight_ <= 0.0) {
        return;
      }
      for (const DeathEvent& event : events) {
        grid_.radius_query(world_.units, event.x, event.y, max_sight_,
                           death_observer_buffer_);
        for (std::uint32_t observer : death_observer_buffer_) {
          Unit& unit = world_.units[observer];
          if (!unit.alive || unit.faction != event.faction) {
            continue;
          }
          const double dx = unit.x - event.x;
          const double dy = unit.y - event.y;
          const double sight = effective_sight(unit);
          if (dx * dx + dy * dy <= sight * sight) {
            counts[observer] += 1;
          }
        }
      }
    };

    // Event-based losses. Deaths happened earlier this tick; routs were
    // recorded last tick (the one-tick delay keeps the contagion independent
    // of unit index order) and are consumed here.
    spread_events(death_events_, ally_death_count_);
    spread_events(rout_events_, ally_rout_count_);
    rout_events_.clear();

    const std::uint64_t tick_number = world_.tick_count + 1;  // 1-based
    std::array<std::uint32_t, kFactionCount> routed_now{};
    std::array<std::uint32_t, kFactionCount> active_now{};

    for (std::size_t i = 0; i < world_.units.size(); ++i) {
      Unit& unit = world_.units[i];
      if (!unit.alive) {
        continue;
      }
      const Perception& perception = last_perception_[i];

      MoraleInputs inputs;
      inputs.enemies_visible = perception.visible_enemies > 0;
      inputs.allies_visible = perception.visible_allies > 0;
      inputs.strength_ratio =
          static_cast<double>(perception.visible_allies + 1) /
          static_cast<double>(perception.visible_allies +
                              perception.visible_enemies + 1);
      inputs.damage_fraction =
          damage_this_tick_[i] / unit.stats.max_health;
      inputs.kills = kills_this_tick_[i];
      inputs.ally_deaths = ally_death_count_[i];
      inputs.ally_routs = ally_rout_count_[i];
      inputs.routed_allies = perception.routed_allies;
      inputs.bravery = std::clamp(unit.personality.bravery, 0.0, 1.0);
      inputs.caution = std::clamp(unit.personality.caution, 0.0, 1.0);
      if (combat_v2_) {
        // m13: rear hits cost extra morale; experience, the defensive
        // stance and a commander's aura resist losses.
        inputs.rear_hits = rear_hits_this_tick_[i];
        inputs.rear_morale_loss =
            effective_combat_->facing.rear_morale_loss;
        double resist = effective_combat_->experience.morale_resist_max *
                        unit.experience;
        if (defensive_stance_[i] != 0) {
          resist += effective_combat_->stance.morale_resist;
        }
        if (aura_active_[i] != 0) {
          resist += effective_combat_->aura.morale_reduction;
        }
        inputs.loss_resist = std::min(resist, 0.9);
      }

      unit.morale =
          std::clamp(unit.morale + morale_delta(inputs, morale), 0.0, 100.0);
      unit.morale_state = static_cast<std::uint8_t>(
          state_from_morale(unit.morale, morale.states));

      // Safe time: consecutive ticks with no enemy in sight.
      safe_ticks_[i] =
          perception.visible_enemies == 0 ? safe_ticks_[i] + 1 : 0;

      const std::size_t faction = faction_index(unit.faction);
      if (!unit.routed &&
          unit.morale < morale.states.shaken_min) {
        // Break: below the breaking value the unit routs; the event is
        // recorded so nearby allies feel the contagion next tick.
        unit.routed = true;
        action_commit_[i] = 0;
        if (events_ != nullptr) {
          events_->record(tick_number, EventType::Rout, unit.id);
        }
        rout_events_.push_back(DeathEvent{unit.x, unit.y, unit.faction});
        RoutingStats& stats = routing_stats_[faction];
        stats.total_routs += 1;
        if (stats.first_rout_tick == 0) {
          stats.first_rout_tick = tick_number;
        }
      } else if (unit.routed &&
                 safe_ticks_[i] >= morale.recovery.safe_ticks &&
                 unit.morale > morale.recovery.morale_threshold) {
        // Recovery: hysteresis — the morale threshold is strictly above the
        // breaking value, plus the safe-time requirement.
        unit.routed = false;
        routing_stats_[faction].recoveries += 1;
        if (events_ != nullptr) {
          events_->record(tick_number, EventType::Recovery, unit.id);
        }
      }

      if (unit.routed) {
        routed_now[faction] += 1;
      } else {
        active_now[faction] += 1;
      }
    }

    for (Faction faction : kAllFactions) {
      const std::size_t index = faction_index(faction);
      RoutingStats& stats = routing_stats_[index];
      stats.peak_routed =
          std::max(stats.peak_routed, routed_now[index]);
      inactive_ticks_[index] =
          active_now[index] == 0 ? inactive_ticks_[index] + 1 : 0;
    }
  }

  // Per-tick scratch reset (both modes: combat accumulates unconditionally).
  // rout_events_ is NOT cleared here: this tick's breaks are only recorded
  // during the loop above and must survive until next tick's spread.
  death_events_.clear();
  std::fill(damage_this_tick_.begin(), damage_this_tick_.end(), 0.0);
  std::fill(kills_this_tick_.begin(), kills_this_tick_.end(), 0);
  std::fill(ally_death_count_.begin(), ally_death_count_.end(), 0);
  std::fill(ally_rout_count_.begin(), ally_rout_count_.end(), 0);
  std::fill(rear_hits_this_tick_.begin(), rear_hits_this_tick_.end(), 0);
  std::fill(attacked_this_tick_.begin(), attacked_this_tick_.end(), 0);
}

// Phase 6 — squads, formations and command (M6; docs/squads.md). Runs after
// deaths and morale so this tick's casualties and routs are already visible.
// Per squad, in global id order: recompute centroid/cohesion, maintain
// command (routed commanders lose it here; deaths were handled in
// apply_deaths), run succession exactly succession_delay_ticks after the
// loss, let the commander re-evaluate its order at most once per
// order_min_duration_ticks, and propagate the order to members (delayed by
// distance inside the leadership radius, decaying outside it). Everything
// iterates fixed member order, so results are deterministic.
void Simulation::update_squads() {
  if (!squads_active_) {
    return;
  }
  const std::uint64_t tick_number = world_.tick_count + 1;  // 1-based

  // m11b: deliver due player orders before the per-squad maintenance
  // (the delay was fixed at issue time; delivery sees the command state
  // as of this tick).
  process_pending_orders(tick_number);

  for (Squad& squad : squads_) {
    update_squad_metrics(squad);

    // Command maintenance: a routed commander loses command (a dead one
    // already lost it in apply_deaths). The loss is permanent — a recovered
    // commander is an ordinary member until (or unless) it succeeds.
    if (squad.command_active) {
      const Unit& commander = world_.units[squad.commander_index];
      if (!commander.alive || commander.routed) {
        squad.command_active = false;
        squad.command_lost_tick = tick_number;
        // m11b: a delivered player order survives command loss (the
        // successor inherits it); only AI orders clear.
        if (!squad.player_order_active) {
          squad.order = Order::None;
          for (SquadMemberState& member : squad.members) {
            member.order = Order::None;
          }
        }
      }
    }

    // Succession: exactly succession_delay_ticks after command was lost,
    // the lowest-unit-id alive, non-routed member takes command (the
    // recovered former commander is eligible like anyone else). If nobody
    // is eligible the squad stays leaderless and retries next tick.
    if (!squad.command_active && squad.command_lost_tick != 0 &&
        tick_number >=
            squad.command_lost_tick + commanders_->succession_delay_ticks) {
      std::size_t successor = world_.units.size();
      std::uint32_t successor_id = 0;
      for (const SquadMemberState& member : squad.members) {
        const Unit& candidate = world_.units[member.unit_index];
        if (!candidate.alive || candidate.routed) {
          continue;
        }
        if (successor == world_.units.size() || candidate.id < successor_id) {
          successor = member.unit_index;
          successor_id = candidate.id;
        }
      }
      if (successor != world_.units.size()) {
        squad.commander_index = static_cast<std::uint32_t>(successor);
        squad.command_active = true;
        squad.command_lost_tick = 0;
        // m11b: a successor inherits a delivered player order; only AI
        // orders reset for a fresh pick.
        if (!squad.player_order_active) {
          squad.order = Order::None;     // the successor picks fresh below
          squad.order_issue_tick = 0;
        }
        squad_stats_[faction_index(squad.faction)].successions += 1;
        if (events_ != nullptr) {
          events_->record(tick_number, EventType::Succession,
                          world_.units[successor].id, squad.id);
        }
      }
    }

    // Order selection: the commander re-evaluates at most once per
    // order_min_duration_ticks, and immediately whenever it has no order.
    // m11b: while a delivered player order is active the commander
    // executes it — the AI does not re-evaluate.
    if (squad.command_active && !squad.player_order_active) {
      const Unit& commander = world_.units[squad.commander_index];
      const bool may_reorder =
          squad.order == Order::None ||
          tick_number >=
              squad.order_issue_tick + commanders_->order_min_duration_ticks;
      if (may_reorder) {
        // Local strength from the commander's own sight-limited perception
        // (the same ratio the utility AI uses for its units).
        grid_.radius_query(world_.units, commander.x, commander.y,
                           effective_sight(commander),
                           squad_perceive_buffer_);
        std::uint32_t allies = 0;
        std::uint32_t enemies = 0;
        for (std::uint32_t candidate : squad_perceive_buffer_) {
          if (candidate == squad.commander_index) {
            continue;  // the commander is not its own ally
          }
          const Unit& other = world_.units[candidate];
          if (other.faction == commander.faction) {
            ++allies;
          } else {
            ++enemies;
          }
        }
        // Squad morale: the mean member morale; a neutral 50 with morale
        // off, so only cohesion and strength drive orders there.
        double squad_morale = 50.0;
        if (morale_enabled_) {
          double morale_sum = 0.0;
          std::size_t alive = 0;
          for (const SquadMemberState& member : squad.members) {
            const Unit& member_unit = world_.units[member.unit_index];
            if (member_unit.alive) {
              morale_sum += member_unit.morale;
              ++alive;
            }
          }
          if (alive > 0) {
            squad_morale = morale_sum / static_cast<double>(alive);
          }
        }
        const Order picked = pick_order(
            squad.cohesion,
            static_cast<double>(allies + 1) /
                static_cast<double>(allies + enemies + 1),
            squad_morale, enemies > 0, commanders_->rules);
        if (picked != squad.order) {
          squad.order = picked;
          squad.order_issue_tick = tick_number;
          squad_stats_[faction_index(squad.faction)]
                      .order_counts[static_cast<std::size_t>(picked)] += 1;
          if (events_ != nullptr) {
            events_->record(tick_number, EventType::OrderChange, squad.id,
                            order_event_value(static_cast<int>(picked)));
          }
        }
      }
    }

    // Order propagation: inside the leadership radius a member adopts the
    // commander's current order once the ticks since it was issued cover
    // the distance delay (ceil(distance * order_delay_per_unit)); the
    // latest order always wins. Outside the radius the last received order
    // decays to None after order_decay_ticks consecutive out-of-range
    // ticks.
    if (squad.command_active) {
      const Unit& commander = world_.units[squad.commander_index];
      for (SquadMemberState& member : squad.members) {
        const Unit& member_unit = world_.units[member.unit_index];
        if (!member_unit.alive) {
          continue;
        }
        const double dx = member_unit.x - commander.x;
        const double dy = member_unit.y - commander.y;
        const double distance = std::sqrt(dx * dx + dy * dy);
        if (distance <= squad.leadership_radius) {
          member.out_of_range_ticks = 0;
          if (member.order != squad.order) {
            const std::uint64_t delay = static_cast<std::uint64_t>(
                std::ceil(distance * commanders_->order_delay_per_unit));
            if (tick_number >= squad.order_issue_tick + delay) {
              member.order = squad.order;
            }
          }
        } else {
          member.out_of_range_ticks += 1;
          if (member.out_of_range_ticks > commanders_->order_decay_ticks) {
            member.order = Order::None;
          }
        }
      }
    }
  }
}

// Phase 7 — statistics, termination and commit. A battle ends when a faction
// is eliminated (the other faction wins; mutual annihilation is a draw), when
// a faction has had no active units for rout_timeout_ticks consecutive ticks
// (M5: all its units dead or routed; the other faction wins, a mutual
// collapse is a draw), or when the tick limit is reached (the faction with
// more survivors wins; an even split is a draw).
void Simulation::update_termination() {
  world_.tick_count += 1;
  // m11b: the per-tick issue budget resets at the end of every tick
  // (issue_order calls between ticks count against the upcoming tick).
  orders_issued_this_tick_ = {};

  const std::uint32_t red = world_.alive_count(Faction::Red);
  const std::uint32_t blue = world_.alive_count(Faction::Blue);

  if (red == 0 || blue == 0) {
    finished_ = true;
    reason_ = TerminationReason::FactionEliminated;
    if (red == 0 && blue == 0) {
      has_winner_ = false;  // mutual annihilation
    } else {
      has_winner_ = true;
      winner_ = red > 0 ? Faction::Red : Faction::Blue;
    }
    return;
  }

  if (morale_enabled_) {
    const std::uint64_t timeout = effective_morale_->rout_timeout_ticks;
    const bool red_routed_out = inactive_ticks_[faction_index(Faction::Red)] >= timeout;
    const bool blue_routed_out =
        inactive_ticks_[faction_index(Faction::Blue)] >= timeout;
    if (red_routed_out || blue_routed_out) {
      finished_ = true;
      reason_ = TerminationReason::FactionRouted;
      if (red_routed_out && blue_routed_out) {
        has_winner_ = false;  // both factions collapsed
      } else {
        has_winner_ = true;
        winner_ = red_routed_out ? Faction::Blue : Faction::Red;
      }
      return;
    }
  }

  // m13 item 2 (--end v2): a faction whose active units (alive and not
  // routed) stayed below max(min_units, fraction of its initial
  // strength) for routed_threshold_ticks consecutive ticks is routed —
  // the mass-rout tail the full-route timeout above never catches (the
  // M13 hold-arm study battles: >90% routed, ~20 fighting on, grinding
  // to the tick limit).
  if (morale_enabled_ && config_.end_v2) {
    const auto active_of = [this](Faction faction) {
      std::uint32_t active = 0;
      for (const Unit& unit : world_.units) {
        if (unit.alive && !unit.routed && unit.faction == faction) {
          ++active;
        }
      }
      return active;
    };
    const auto threshold_out = [&](Faction faction) {
      const std::size_t index = faction_index(faction);
      const auto& term = effective_morale_->termination;
      const std::uint32_t threshold = std::max(
          term.routed_threshold_min_units,
          static_cast<std::uint32_t>(
              term.routed_threshold_fraction *
              static_cast<double>(initial_alive_[index])));
      if (active_of(faction) >= threshold) {
        below_threshold_ticks_[index] = 0;
        return false;
      }
      below_threshold_ticks_[index] += 1;
      return below_threshold_ticks_[index] >= term.routed_threshold_ticks;
    };
    const bool red_threshold = threshold_out(Faction::Red);
    const bool blue_threshold = threshold_out(Faction::Blue);
    if (red_threshold || blue_threshold) {
      finished_ = true;
      reason_ = TerminationReason::RoutedThreshold;
      if (red_threshold && blue_threshold) {
        has_winner_ = false;  // both factions below the threshold
      } else {
        has_winner_ = true;
        winner_ = red_threshold ? Faction::Blue : Faction::Red;
      }
      return;
    }
  }

  // m13 item 2 (--end v2, stalemate): after first contact, track the
  // combined attacker count per tick; when the last stalemate_ticks
  // ticks average fewer than stalemate_max_engagements attackers, the
  // battle is a rout-lock standoff — two mostly-routed armies trading
  // potshots or not fighting at all (the M12-documented Nile swapped
  // tail: 0-3 attackers per tick with rare bursts, versus 30-60+ in a
  // real fight). An average, not a streak, so sporadic bursts cannot
  // keep a dead battle alive. The first_contact guard keeps pre-contact
  // marches out of the window; winner by alive count, like the tick
  // limit.
  if (morale_enabled_ && config_.end_v2 && first_contact_tick_ > 0 &&
      !attack_count_ring_.empty()) {
    std::uint32_t attacking = 0;
    for (const Unit& unit : world_.units) {
      if (unit.alive &&
          unit.last_action == static_cast<std::uint8_t>(Action::Attack)) {
        ++attacking;
      }
    }
    const std::size_t window = attack_count_ring_.size();
    attack_count_ring_[stalemate_ring_pos_] = attacking;
    stalemate_ring_pos_ = (stalemate_ring_pos_ + 1) % window;
    stalemate_samples_ = std::min(stalemate_samples_ + 1, window);
    if (stalemate_samples_ >= window) {
      std::uint64_t attacks = 0;
      for (std::uint32_t count : attack_count_ring_) {
        attacks += count;
      }
      if (attacks < static_cast<std::uint64_t>(
                       effective_morale_->termination
                           .stalemate_max_engagements) *
                       static_cast<std::uint64_t>(window)) {
        finished_ = true;
        reason_ = TerminationReason::Stalemate;
        if (red != blue) {
          has_winner_ = true;
          winner_ = red > blue ? Faction::Red : Faction::Blue;
        } else {
          has_winner_ = false;
        }
        return;
      }
    }
  }

  if (world_.tick_count >= config_.max_ticks) {
    finished_ = true;
    reason_ = TerminationReason::TickLimit;
    if (red != blue) {
      has_winner_ = true;
      winner_ = red > blue ? Faction::Red : Faction::Blue;
    } else {
      has_winner_ = false;  // even survivors: draw
    }
  }
}

// --- Terrain (M8) ---------------------------------------------------------

double Simulation::effective_sight(const Unit& unit) const noexcept {
  if (terrain_ == nullptr) {
    return unit.stats.sight_radius;
  }
  return unit.stats.sight_radius *
         terrain_->sight_scale_at(unit.x, unit.y,
                                  config_.terrain_sight_max_scale);
}

bool Simulation::flow_step(std::size_t faction, double x, double y,
                           double& dx, double& dy) const noexcept {
  if (!terrain_->in_bounds(x, y)) {
    return false;
  }
  const std::uint32_t cx =
      static_cast<std::uint32_t>(x / terrain_->cell_size);
  const std::uint32_t cy =
      static_cast<std::uint32_t>(y / terrain_->cell_size);
  return flow_direction(flow_[faction], cx, cy, dx, dy);
}

bool Simulation::snap_to_passable(double& x, double& y) const noexcept {
  const TerrainGrid& grid = *terrain_;
  if (!grid.in_bounds(x, y)) {
    return false;
  }
  const std::uint32_t cx = static_cast<std::uint32_t>(x / grid.cell_size);
  const std::uint32_t cy = static_cast<std::uint32_t>(y / grid.cell_size);
  for (std::uint32_t ring = 1; ring <= 16; ++ring) {
    for (std::int32_t oy = -static_cast<std::int32_t>(ring);
         oy <= static_cast<std::int32_t>(ring); ++oy) {
      for (std::int32_t ox = -static_cast<std::int32_t>(ring);
           ox <= static_cast<std::int32_t>(ring); ++ox) {
        if (std::max(std::abs(ox), std::abs(oy)) !=
            static_cast<std::int32_t>(ring)) {
          continue;  // ring cells only, row-major order (deterministic)
        }
        const std::int32_t nx = static_cast<std::int32_t>(cx) + ox;
        const std::int32_t ny = static_cast<std::int32_t>(cy) + oy;
        if (nx < 0 || ny < 0 || nx >= static_cast<std::int32_t>(grid.width) ||
            ny >= static_cast<std::int32_t>(grid.height)) {
          continue;
        }
        const double px =
            (static_cast<std::uint32_t>(nx) + 0.5) * grid.cell_size;
        const double py =
            (static_cast<std::uint32_t>(ny) + 0.5) * grid.cell_size;
        if (grid.passable_at(px, py)) {
          x = px;
          y = py;
          return true;
        }
      }
    }
  }
  return false;
}

bool Simulation::formation_slot_world(const Squad& squad,
                                      std::size_t member_index, double& x,
                                      double& y) const noexcept {
  double anchor_x = squad.centroid_x;
  double anchor_y = squad.centroid_y;
  if (squad.command_active) {
    const Unit& commander = world_.units[squad.commander_index];
    anchor_x = commander.x;
    anchor_y = commander.y;
  }
  const SlotOffset slot = formation_slot(squad.formation, squad.members.size(),
                                         squad.spacing, member_index);
  const std::size_t faction = static_cast<std::size_t>(squad.faction);
  x = anchor_x + forward_x_[faction] * slot.forward +
      lateral_x_[faction] * slot.lateral;
  y = anchor_y + forward_y_[faction] * slot.forward +
      lateral_y_[faction] * slot.lateral;
  // Formation slots never sit inside impassable terrain: nudge to the
  // nearest passable cell center (fixed ring scan; M8).
  if (terrain_ != nullptr && !terrain_->passable_at(x, y)) {
    snap_to_passable(x, y);
  }
  return true;
}

bool Simulation::squad_slot_position(std::int32_t squad_id,
                                     std::size_t member_index, double& x,
                                     double& y) const noexcept {
  if (!squads_active_ || squad_id < 0 ||
      static_cast<std::size_t>(squad_id) >= squads_.size()) {
    return false;
  }
  const Squad& squad = squads_[static_cast<std::size_t>(squad_id)];
  if (member_index >= squad.members.size()) {
    return false;
  }
  return formation_slot_world(squad, member_index, x, y);
}

double Simulation::commit_move_terrain(std::size_t index,
                                       const PendingMove& move) {
  Unit& unit = world_.units[index];
  const double old_x = unit.x;
  const double old_y = unit.y;
  const double dx = move.x - unit.x;
  const double dy = move.y - unit.y;
  const double intended = std::sqrt(dx * dx + dy * dy);
  double moved = 0.0;

  // Full step, scaled by the speed multiplier of the cell it enters.
  const double full =
      terrain_->speed_multiplier_at(unit.x + dx, unit.y + dy);
  if (full > 0.0) {
    const double sx = dx * full;
    const double sy = dy * full;
    if (terrain_->passable_at(unit.x + sx, unit.y + sy)) {
      unit.x += sx;
      unit.y += sy;
      moved = std::sqrt(sx * sx + sy * sy);
    }
  }
  if (moved == 0.0 && (dx != 0.0 || dy != 0.0)) {
    // Wall sliding: each axis alone, scaled by its own destination's
    // multiplier; a blocked axis contributes nothing, and a fully blocked
    // unit stays (its cell is passable by the invariant).
    if (dx != 0.0) {
      const double mx =
          terrain_->speed_multiplier_at(unit.x + dx, unit.y);
      if (mx > 0.0 && terrain_->passable_at(unit.x + dx * mx, unit.y)) {
        unit.x += dx * mx;
      }
    }
    if (dy != 0.0) {
      const double my =
          terrain_->speed_multiplier_at(unit.x, unit.y + dy);
      if (my > 0.0 && terrain_->passable_at(unit.x, unit.y + dy * my)) {
        unit.y += dy * my;
      }
    }
    const double ax = unit.x - old_x;
    const double ay = unit.y - old_y;
    moved = std::sqrt(ax * ax + ay * ay);
  }

  unit.last_move_x = unit.x - old_x;
  unit.last_move_y = unit.y - old_y;

  // Stuck bookkeeping (goal-seekers only): less than kStuckEpsilon actual
  // progress while intending more counts one stuck tick; after
  // kStuckTickThreshold of them the unit follows the flow field for
  // kFlowFollowTicks ticks (resolve_movement reads flow_follow_until_).
  if (move.goal_seeking && intended > kStuckEpsilon) {
    if (moved < kStuckEpsilon) {
      if (stuck_ticks_[index] < 0xFFFF) {
        ++stuck_ticks_[index];
      }
      if (stuck_ticks_[index] > max_stuck_stretch_[index]) {
        max_stuck_stretch_[index] = stuck_ticks_[index];
      }
      if (max_stuck_stretch_[index] > 100 && !counted_stuck_[index]) {
        counted_stuck_[index] = true;
        ++units_stuck_over_100_;
      }
      if (stuck_ticks_[index] >= kStuckTickThreshold) {
        flow_follow_until_[index] =
            world_.tick_count + kFlowFollowTicks;
        stuck_ticks_[index] = 0;
      }
    } else {
      stuck_ticks_[index] = 0;
    }
  }
  return moved;
}

void Simulation::refresh_flow_fields() {
  for (Faction faction : kAllFactions) {
    const std::size_t index = faction_index(faction);
    const double ox = objective_x_[index];
    const double oy = objective_y_[index];
    if (!terrain_->in_bounds(ox, oy)) {
      continue;  // off-map objectives keep the straight-line Advance
    }
    const std::uint64_t cell =
        static_cast<std::uint64_t>(oy / terrain_->cell_size) *
            terrain_->width +
        static_cast<std::uint32_t>(ox / terrain_->cell_size);
    if (cell == flow_target_cell_[index]) {
      continue;  // cached: same objective cell
    }
    flow_[index] = compute_flow_field(
        *terrain_, static_cast<std::uint32_t>(ox / terrain_->cell_size),
        static_cast<std::uint32_t>(oy / terrain_->cell_size));
    flow_target_cell_[index] = cell;
  }
}

void Simulation::init_terrain() {
  const TerrainGrid& grid = *terrain_;
  // The battlefield must be the map (size x cell_size).
  if (std::fabs(world_.width - grid.width * grid.cell_size) > 1e-6 ||
      std::fabs(world_.height - grid.height * grid.cell_size) > 1e-6) {
    throw std::invalid_argument(
        "terrain map '" + grid.map_id + "': battlefield " +
        std::to_string(world_.width) + "x" + std::to_string(world_.height) +
        " does not match the map (" +
        std::to_string(grid.width * grid.cell_size) + "x" +
        std::to_string(grid.height * grid.cell_size) + ")");
  }
  // No unit may start inside impassable terrain (explicit states
  // included; spawn_armies already resamples generated positions).
  for (const Unit& unit : world_.units) {
    if (unit.alive && !grid.passable_at(unit.x, unit.y)) {
      throw std::invalid_argument("unit " + std::to_string(unit.id) +
                                  " starts inside impassable terrain");
    }
  }
  stuck_ticks_.assign(world_.units.size(), 0);
  max_stuck_stretch_.assign(world_.units.size(), 0);
  counted_stuck_.assign(world_.units.size(), false);
  flow_follow_until_.assign(world_.units.size(), 0);
  terrain_kills_.assign(grid.table->types.size(), 0);
  flow_target_cell_.fill(~std::uint64_t{0});
  refresh_flow_fields();

  // Objectives must sit on passable ground.
  for (Faction faction : kAllFactions) {
    const std::size_t index = faction_index(faction);
    if (!grid.passable_at(objective_x_[index], objective_y_[index])) {
      throw std::invalid_argument(std::string("objective for ") +
                                  to_string(faction) +
                                  " sits on impassable terrain");
    }
  }
  // Each spawn band needs passable cells, all reachable from the
  // faction objective (no sealed-off spawns).
  const double band = world_.width * config_.spawn_band_fraction;
  for (Faction faction : kAllFactions) {
    const std::size_t index = faction_index(faction);
    const std::uint32_t x_begin =
        faction == Faction::Red
            ? 0
            : static_cast<std::uint32_t>(
                  (world_.width - band) / grid.cell_size);
    const std::uint32_t x_end =
        faction == Faction::Red
            ? static_cast<std::uint32_t>(band / grid.cell_size) + 1
            : grid.width;
    std::uint32_t passable = 0;
    std::uint32_t unreachable = 0;
    for (std::uint32_t cy = 0; cy < grid.height; ++cy) {
      for (std::uint32_t cx = x_begin; cx < x_end && cx < grid.width; ++cx) {
        const double px = (cx + 0.5) * grid.cell_size;
        const double py = (cy + 0.5) * grid.cell_size;
        if (!grid.passable_at(px, py)) {
          continue;
        }
        ++passable;
        if (flow_[index].cost[static_cast<std::size_t>(cy) * grid.width +
                              cx] >= FlowField::kUnreached) {
          ++unreachable;
        }
      }
    }
    if (passable == 0) {
      throw std::invalid_argument(std::string("spawn band for ") +
                                  to_string(faction) +
                                  " has no passable cells");
    }
    if (unreachable > 0) {
      throw std::invalid_argument(
          std::string("objective for ") + to_string(faction) +
          " is unreachable from " + std::to_string(unreachable) +
          " passable spawn cells");
    }
  }
}

}  // namespace sim
