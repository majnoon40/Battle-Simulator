// M10 Part 2b: canonical state hashing for replay verification.
// Appends every piece of mutable state field-by-field (no struct
// memcpy, so padding bytes can never leak in) and folds the bytes with
// two independent FNV-1a 64 seeds (a 128-bit digest in practice).
//
// Canonical byte encoding (m13 audit finding 5): every integer is
// serialized explicitly little-endian and every double as its exact
// IEEE-754 bit pattern, little-endian — never in the host's native
// order. On every supported platform (x86-64, ARM64 in little-endian
// mode) these are the native representations, so all existing hash
// values — replay checkpoints, committed baselines, recorded replays —
// are byte-for-byte unchanged; a hypothetical big-endian host now
// produces the same digest instead of silently diverging (and failing
// cross-verification). The format is part of the replay compatibility
// guarantee: changing any encoding below changes every recorded
// replay's verification and is a versioned change.
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "sim/simulation.hpp"

namespace sim {
namespace {

// Little-endian integer encoders: the low byte first, by explicit
// shifts (no reinterpretation of the object representation).
void u8(std::string& out, std::uint8_t v) {
  out.push_back(static_cast<char>(v));
}
void u16(std::string& out, std::uint16_t v) {
  u8(out, static_cast<std::uint8_t>(v & 0xFF));
  u8(out, static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void u32(std::string& out, std::uint32_t v) {
  u16(out, static_cast<std::uint16_t>(v & 0xFFFF));
  u16(out, static_cast<std::uint16_t>((v >> 16) & 0xFFFF));
}
void u64(std::string& out, std::uint64_t v) {
  u32(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFULL));
  u32(out, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFULL));
}
// Two's-complement little-endian via the unsigned conversion (defined
// behavior in C++20).
void i32(std::string& out, int v) {
  u32(out, static_cast<std::uint32_t>(v));
}
// The exact IEEE-754 bit pattern of the double, little-endian. The bit
// pattern (not a decimal round-trip) keeps -0.0, subnormals and every
// value the deterministic simulation produces perfectly distinct.
void f64(std::string& out, double v) {
  static_assert(sizeof(std::uint64_t) == sizeof(double),
                "the canonical double encoding needs 64-bit doubles");
  std::uint64_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  u64(out, bits);
}
void boolean(std::string& out, bool v) { u8(out, v ? 1 : 0); }

void stats_bytes(std::string& out, const UnitStats& s) {
  f64(out, s.max_health);
  f64(out, s.speed);
  f64(out, s.damage);
  f64(out, s.attack_range);
  u32(out, s.attack_cooldown_ticks);
  f64(out, s.sight_radius);
  f64(out, s.accuracy);
  f64(out, s.damage_variance);
  f64(out, s.cooldown_jitter);
}

void unit_bytes(std::string& out, const Unit& u) {
  u32(out, u.id);
  u8(out, static_cast<std::uint8_t>(u.faction));
  u32(out, u.type_index);
  f64(out, u.x);
  f64(out, u.y);
  f64(out, u.health);
  stats_bytes(out, u.stats);
  u32(out, u.cooldown_remaining);
  f64(out, u.personality.bravery);
  f64(out, u.personality.caution);
  f64(out, u.personality.aggression);
  u8(out, u.personality.archetype);
  u8(out, u.last_action);
  f64(out, u.last_move_x);
  f64(out, u.last_move_y);
  f64(out, u.facing);      // m13 item 3 (0.0 under --combat v1)
  f64(out, u.fatigue);
  f64(out, u.experience);
  f64(out, u.morale);
  u8(out, u.morale_state);
  boolean(out, u.routed);
  boolean(out, u.alive);
  f64(out, u.memory_x);
  f64(out, u.memory_y);
  u64(out, u.memory_tick);
  u32(out, u.pursuit_ticks);
  u32(out, u.pursuit_target);
  f64(out, u.pursuit_anchor_x);
  f64(out, u.pursuit_anchor_y);
  u8(out, u.decision_interval);
}

// Size-guarded per-unit readers: vectors that only exist for some
// feature combinations (terrain, squads, tactics) read as zeros when
// the feature was off, so the hash never reads out of bounds and stays
// defined for every battle type.
template <typename T>
T at_or(const std::vector<T>& v, std::size_t i, T fallback = T{}) {
  return i < v.size() ? v[i] : fallback;
}

void perception_bytes(std::string& out, const Perception& p) {
  i32(out, p.nearest_visible_enemy);
  f64(out, p.nearest_visible_enemy_distance);
  u32(out, p.visible_enemies);
  u32(out, p.visible_allies);
  u32(out, p.routed_allies);
  f64(out, p.enemy_centroid_x);
  f64(out, p.enemy_centroid_y);
}

}  // namespace

std::string Simulation::state_hash() const {
  std::string bytes;
  bytes.reserve(4096);

  // Battle-wide scalars.
  u64(bytes, world_.tick_count);
  f64(bytes, world_.width);
  f64(bytes, world_.height);
  u64(bytes, rng_.state());
  u64(bytes, config_.seed);
  u8(bytes, static_cast<std::uint8_t>(reason_));
  boolean(bytes, finished_);
  boolean(bytes, has_winner_);
  u8(bytes, static_cast<std::uint8_t>(winner_));

  // Objectives and faction-level state.
  for (std::size_t f = 0; f < faction_count_; ++f) {
    f64(bytes, objective_x_[f]);
    f64(bytes, objective_y_[f]);
    u64(bytes, faction_reports_[f].initial);
    u64(bytes, faction_reports_[f].deaths);
    u64(bytes, inactive_ticks_[f]);
    u64(bytes, routing_stats_[f].total_routs);
    u64(bytes, routing_stats_[f].recoveries);
    u64(bytes, routing_stats_[f].first_rout_tick);
    for (std::uint64_t count : action_counts_[f]) {
      u64(bytes, count);
    }
    for (std::uint64_t count : squad_stats_[f].order_counts) {
      u64(bytes, count);
    }
    u64(bytes, squad_stats_[f].successions);
    u64(bytes, squad_stats_[f].commander_deaths);
    u64(bytes, squad_stats_[f].drops_from_formation);
    u32(bytes, squad_stats_[f].squads);
    u64(bytes, faction_reports_[f].combat.flank_hits);
    u64(bytes, faction_reports_[f].combat.rear_hits);
    u64(bytes, faction_reports_[f].combat.charge_hits);
    u64(bytes, faction_reports_[f].combat.stance_hits_taken);
    f64(bytes, forward_x_[f]);
    f64(bytes, forward_y_[f]);
    f64(bytes, lateral_x_[f]);
    f64(bytes, lateral_y_[f]);
    f64(bytes, estimate_x_[f]);
    f64(bytes, estimate_y_[f]);
    u64(bytes, estimate_tick_[f]);
    u64(bytes, flow_target_cell_[f]);
    // Type reports (kills + damage influence nothing, but they are
    // recorded state — a divergence here still means "not identical").
    u32(bytes, static_cast<std::uint32_t>(type_reports_[f].size()));
    for (const TypeReport& report : type_reports_[f]) {
      u32(bytes, report.spawned);
      u32(bytes, report.kills);
      u64(bytes, report.damage_dealt);
      u32(bytes, report.survivors);
    }
  }

  // Per-unit state.
  u32(bytes, static_cast<std::uint32_t>(world_.units.size()));
  for (const Unit& unit : world_.units) {
    unit_bytes(bytes, unit);
  }
  for (std::size_t i = 0; i < world_.units.size(); ++i) {
    i32(bytes, at_or(target_index_, i, -1));
    if (i < last_perception_.size()) {
      perception_bytes(bytes, last_perception_[i]);
    } else {
      Perception empty{};
      perception_bytes(bytes, empty);
    }
    u8(bytes, at_or(action_commit_, i));
    u32(bytes, at_or(contactless_streak_, i));
    u8(bytes, at_or(was_seeking_, i));
    u16(bytes, at_or(stuck_ticks_, i));
    u16(bytes, at_or(max_stuck_stretch_, i));
    boolean(bytes, at_or(counted_stuck_, i));
    u64(bytes, at_or(flow_follow_until_, i));
    f64(bytes, at_or(tactic_goal_x_, i));
    f64(bytes, at_or(tactic_goal_y_, i));
    u8(bytes, at_or(tactic_goal_set_, i));
    i32(bytes, at_or(unit_squad_, i, -1));
    u32(bytes, at_or(unit_member_, i));
  }

  // Per-tick morale buffers (usually zeroed at tick boundaries; they
  // are still state, so they are covered).
  for (double damage : damage_this_tick_) {
    f64(bytes, damage);
  }
  for (std::uint32_t kills : kills_this_tick_) {
    u32(bytes, kills);
  }
  for (std::uint64_t safe : safe_ticks_) {
    u64(bytes, safe);
  }
  for (std::uint32_t deaths : ally_death_count_) {
    u32(bytes, deaths);
  }
  for (std::uint32_t routs : ally_rout_count_) {
    u32(bytes, routs);
  }
  for (std::uint16_t kill : terrain_kills_) {
    u16(bytes, kill);
  }
  // m13 item 3 (--combat v2): per-tick combat-depth buffers (zeros
  // under v1) and the charge run ring.
  for (std::uint8_t aura : aura_active_) {
    u8(bytes, aura);
  }
  for (std::uint8_t stance : defensive_stance_) {
    u8(bytes, stance);
  }
  for (std::uint32_t rear : rear_hits_this_tick_) {
    u32(bytes, rear);
  }
  for (std::uint32_t attacked : attacked_this_tick_) {
    u8(bytes, static_cast<std::uint8_t>(attacked));
  }
  for (double move : charge_ring_) {
    f64(bytes, move);
  }
  for (std::uint32_t pos : charge_ring_pos_) {
    u32(bytes, pos);
  }
  u32(bytes, units_stuck_over_100_);

  // Death/rout events still visible to next tick's morale contagion.
  u32(bytes, static_cast<std::uint32_t>(death_events_.size()));
  for (const DeathEvent& event : death_events_) {
    f64(bytes, event.x);
    f64(bytes, event.y);
    u8(bytes, static_cast<std::uint8_t>(event.faction));
  }
  u32(bytes, static_cast<std::uint32_t>(rout_events_.size()));
  for (const DeathEvent& event : rout_events_) {
    f64(bytes, event.x);
    f64(bytes, event.y);
    u8(bytes, static_cast<std::uint8_t>(event.faction));
  }

  // Squads.
  u32(bytes, static_cast<std::uint32_t>(squads_.size()));
  for (std::size_t index = 0; index < squads_.size(); ++index) {
    const Squad& squad = squads_[index];
    u32(bytes, squad.id);
    u8(bytes, static_cast<std::uint8_t>(squad.faction));
    u8(bytes, static_cast<std::uint8_t>(squad.formation));
    f64(bytes, squad.spacing);
    f64(bytes, squad.leadership_radius);
    u32(bytes, squad.commander_index);
    boolean(bytes, squad.command_active);
    u64(bytes, squad.command_lost_tick);
    u8(bytes, static_cast<std::uint8_t>(squad.order));
    u64(bytes, squad.order_issue_tick);
    f64(bytes, squad.cohesion);
    f64(bytes, squad.centroid_x);
    f64(bytes, squad.centroid_y);
    u32(bytes, squad.contact_count);
    for (const SquadContact& contact : squad.contacts) {
      f64(bytes, contact.x);
      f64(bytes, contact.y);
      u64(bytes, contact.tick);
    }
    u32(bytes, static_cast<std::uint32_t>(squad.members.size()));
    for (const SquadMemberState& member : squad.members) {
      u32(bytes, member.unit_index);
      u8(bytes, static_cast<std::uint8_t>(member.order));
      u32(bytes, member.out_of_range_ticks);
      boolean(bytes, member.in_formation);
    }
    u8(bytes, at_or(squad_engaged_, index));
    // m11b player-order state.
    boolean(bytes, squad.player_order_active);
    u8(bytes, static_cast<std::uint8_t>(squad.player_order_type));
    boolean(bytes, squad.player_order_point);
    f64(bytes, squad.player_order_x);
    f64(bytes, squad.player_order_y);
    u64(bytes, at_or(squad_flow_target_, index,
                     std::numeric_limits<std::uint64_t>::max()));
  }

  // m11b: in-flight player orders (order matters — delivery order is
  // queue order) and the report counters.
  u32(bytes, static_cast<std::uint32_t>(pending_orders_.size()));
  for (const PlayerOrder& order : pending_orders_) {
    u64(bytes, order.issue_tick);
    u64(bytes, order.deliver_tick);
    u8(bytes, static_cast<std::uint8_t>(order.faction));
    u32(bytes, order.squad_id);
    u8(bytes, static_cast<std::uint8_t>(order.type));
    f64(bytes, order.x);
    f64(bytes, order.y);
    boolean(bytes, order.has_point);
    boolean(bytes, order.formation_set);
    u8(bytes, static_cast<std::uint8_t>(order.formation));
    f64(bytes, order.formation_spacing);
  }
  for (std::size_t f = 0; f < faction_count_; ++f) {
    const Simulation::PlayerOrderReport& report = player_order_reports_[f];
    u64(bytes, report.issued);
    u64(bytes, report.delivered);
    u64(bytes, report.rejected);
    u64(bytes, report.dropped);
    u64(bytes, report.delay_ticks_sum);
    f64(bytes, report.obedience_sum);
  }

  // FNV-1a 128: two independent 64-bit lanes over the same bytes.
  std::uint64_t h1 = 0xcbf29ce484222325ULL;
  std::uint64_t h2 = 0x84222325cbf29ce4ULL;
  for (unsigned char c : bytes) {
    h1 = (h1 ^ c) * 0x100000001b3ULL;
    h2 = (h2 ^ c) * 0x100000001b3ULL;
  }
  char hex[33];
  std::snprintf(hex, sizeof(hex), "%016llx%016llx",
                static_cast<unsigned long long>(h1),
                static_cast<unsigned long long>(h2));
  return std::string(hex, 32);
}

}  // namespace sim
