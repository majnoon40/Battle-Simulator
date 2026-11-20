// M15 S1: the invariant checker. See Simulation::enable_invariant_checks
// in sim/simulation.hpp for the contract. Everything here only READS
// simulation state (the mutable id scratch is reused, so a healthy check
// allocates nothing); the one-shot check_invariants() returns violations,
// run_invariant_check() records and freezes. The InvariantCorruption
// functions at the bottom are the test seam — they perform exactly the
// one private write they name, nothing else, and no production code path
// ever calls them.

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "sim/invariants.hpp"
#include "sim/simulation.hpp"
#include "sim/terrain.hpp"

namespace sim {

namespace {

// A wildly corrupt state can produce thousands of violations; the first
// 64 tell the story and keep the record bounded.
constexpr std::size_t kMaxRecordedViolations = 64;

[[nodiscard]] bool finite(double v) noexcept { return std::isfinite(v); }

[[nodiscard]] std::string at(double v) {
  return finite(v) ? std::to_string(v) : std::string("non-finite");
}

}  // namespace

void Simulation::enable_invariant_checks(std::uint32_t every_n_ticks) {
  if (every_n_ticks == 0) {
    throw std::invalid_argument(
        "enable_invariant_checks: interval must be at least 1");
  }
  invariant_interval_ = every_n_ticks;
  invariant_seen_ids_.clear();
  invariant_seen_ids_.reserve(world_.units.size() + 8);
  invariant_dead_at_tick_start_.resize(world_.units.size());
  // Validate the spawn (or current) state right away: a battle that
  // starts corrupt never runs a tick.
  run_invariant_check();
}

void Simulation::snapshot_invariant_dead() {
  invariant_dead_at_tick_start_.resize(world_.units.size());
  for (std::size_t i = 0; i < world_.units.size(); ++i) {
    invariant_dead_at_tick_start_[i] = world_.units[i].alive ? 0 : 1;
  }
  invariant_dead_snapshot_tick_ = world_.tick_count + 1;
}

void Simulation::run_invariant_check() {
  if (invariant_failed_) {
    return;  // already frozen; the first violations stay recorded
  }
  const std::vector<InvariantViolation> found = check_invariants();
  for (const InvariantViolation& violation : found) {
    if (invariant_violations_.size() >= kMaxRecordedViolations) {
      break;
    }
    invariant_violations_.push_back(violation);
  }
  if (!invariant_violations_.empty()) {
    invariant_failed_ = true;
  }
}

std::vector<InvariantViolation> Simulation::check_invariants() const {
  std::vector<InvariantViolation> out;
  const std::uint64_t tick = world_.tick_count;
  const auto add = [&](const char* invariant, std::size_t index,
                       std::string detail) {
    if (out.size() >= kMaxRecordedViolations) {
      return;
    }
    InvariantViolation violation;
    violation.tick = tick;
    if (index != InvariantViolation::kNoUnit &&
        index < world_.units.size()) {
      violation.unit_index = index;
      violation.unit_id = world_.units[index].id;
    }
    violation.invariant = invariant;
    violation.detail = std::move(detail);
    out.push_back(std::move(violation));
  };

  // ---- faction count ------------------------------------------------
  if (faction_count_ < kFactionCount || faction_count_ > kMaxFactions) {
    add("faction-count", InvariantViolation::kNoUnit,
        "faction_count_ " + std::to_string(faction_count_) +
            " outside [2, " + std::to_string(kMaxFactions) + "]");
  }

  // ---- ids: non-zero and unique --------------------------------------
  invariant_seen_ids_.clear();
  for (std::size_t i = 0; i < world_.units.size(); ++i) {
    const Unit& unit = world_.units[i];
    if (unit.id == 0) {
      add("id-nonzero", i,
          "unit at index " + std::to_string(i) + " has the reserved id 0");
    } else {
      invariant_seen_ids_.push_back(unit.id);
    }
  }
  std::sort(invariant_seen_ids_.begin(), invariant_seen_ids_.end());
  for (std::size_t j = 1; j < invariant_seen_ids_.size(); ++j) {
    if (invariant_seen_ids_[j] == invariant_seen_ids_[j - 1]) {
      const Unit* first = world_.find_by_id(invariant_seen_ids_[j]);
      add("id-unique",
          first != nullptr
              ? static_cast<std::size_t>(first - world_.units.data())
              : InvariantViolation::kNoUnit,
          "id " + std::to_string(invariant_seen_ids_[j]) +
              " appears more than once");
    }
  }

  // Was this unit index dead at the start of the last snapshotted tick?
  // (Deaths are permanent, so a past snapshot stays a valid lower bound
  // on the dead set.) No snapshot yet — or the unit unknown to it —
  // means the target checks below are skipped for it: a unit that died
  // during the tick legitimately keeps its last target slot until the
  // next decide phase resets target_index_ (hashed state; clearing it
  // at death would change every replay hash).
  const auto dead_at_snapshot = [this](std::size_t index) {
    return invariant_dead_snapshot_tick_ != kNoSnapshotTick &&
           index < invariant_dead_at_tick_start_.size() &&
           invariant_dead_at_tick_start_[index] != 0;
  };

  // ---- per-unit state -------------------------------------------------
  for (std::size_t i = 0; i < world_.units.size(); ++i) {
    const Unit& unit = world_.units[i];

    // Position: finite; alive units never inside an impassable cell
    // (movement wall-slides, spawn snaps; a dead unit's last cell is
    // history and not checked). Bounds are NOT enforced yet — see the
    // finding in docs/milestone-15.md: the Advance/Approach/Retreat
    // move paths never clamp, so a fleeing unit can legitimately stand
    // past the border until the move-clamp fix lands with its flag.
    if (!finite(unit.x) || !finite(unit.y)) {
      add("position-finite", i,
          "position (" + at(unit.x) + ", " + at(unit.y) + ") is not finite");
    } else if (unit.alive && terrain_ != nullptr &&
               !terrain_->passable_at(unit.x, unit.y)) {
      add("position-passable", i,
          "alive unit inside an impassable cell at (" + at(unit.x) + ", " +
              at(unit.y) + ")");
    }

    // Health: finite, alive in (0, max], dead exactly 0 (apply_deaths
    // zeroes both health and cooldown).
    if (!finite(unit.health) || !finite(unit.stats.max_health)) {
      add("health-finite", i,
          "health " + at(unit.health) + " / max " +
              at(unit.stats.max_health) + " is not finite");
    } else if (unit.stats.max_health <= 0.0) {
      add("health-max", i, "max_health " + at(unit.stats.max_health) +
                               " is not positive");
    } else if (unit.alive && (unit.health <= 0.0 ||
                              unit.health > unit.stats.max_health)) {
      add("health-range", i,
          "alive unit health " + at(unit.health) + " outside (0, " +
              at(unit.stats.max_health) + "]");
    } else if (!unit.alive && unit.health != 0.0) {
      add("health-dead", i,
          "dead unit health " + at(unit.health) + " != 0");
    }

    // Morale, fatigue, experience, facing.
    if (!finite(unit.morale) || unit.morale < 0.0 || unit.morale > 100.0) {
      add("morale-range", i, "morale " + at(unit.morale) + " outside [0, 100]");
    }
    if (unit.morale_state >
        static_cast<std::uint8_t>(MoraleState::Broken)) {
      add("morale-state", i, "morale_state " +
                                 std::to_string(unit.morale_state) +
                                 " has no MoraleState value");
    }
    if (!finite(unit.fatigue) || unit.fatigue < 0.0 || unit.fatigue > 1.0) {
      add("fatigue-range", i,
          "fatigue " + at(unit.fatigue) + " outside [0, 1]");
    }
    if (!finite(unit.experience) || unit.experience < 0.0 ||
        unit.experience > 1.0) {
      add("experience-range", i,
          "experience " + at(unit.experience) + " outside [0, 1]");
    }
    if (!finite(unit.facing)) {
      add("facing-finite", i, "facing " + at(unit.facing) + " is not finite");
    }

    // Faction: one this battle fields.
    if (static_cast<std::size_t>(unit.faction) >= faction_count_) {
      add("faction-valid", i,
          "faction index " +
              std::to_string(static_cast<unsigned>(unit.faction)) +
              " not among the " + std::to_string(faction_count_) +
              " fielded factions");
    }

    // Dead units carry no target (after the one-tick grace above: a
    // unit dead before the tick began has been through a decide reset)
    // and no cooldown (apply_deaths zeroes it immediately).
    if (!unit.alive) {
      if (dead_at_snapshot(i) && i < target_index_.size() &&
          target_index_[i] != -1) {
        add("dead-no-target", i,
            "dead unit still has target index " +
                std::to_string(target_index_[i]));
      }
      if (unit.cooldown_remaining != 0) {
        add("dead-no-cooldown", i,
            "dead unit has cooldown " +
                std::to_string(unit.cooldown_remaining) + " != 0");
      }
    }

    // Targets of alive units: valid index, a living enemy (a target
    // that died during this same tick is the one-tick grace above; one
    // dead before the tick began must not still be targeted).
    if (unit.alive && i < target_index_.size() && target_index_[i] >= 0) {
      const int target = target_index_[i];
      if (static_cast<std::size_t>(target) >= world_.units.size()) {
        add("target-index", i,
            "target index " + std::to_string(target) + " out of range");
      } else {
        const Unit& target_unit =
            world_.units[static_cast<std::size_t>(target)];
        if (!target_unit.alive && dead_at_snapshot(target)) {
          add("target-alive", i,
              "targets dead unit " + std::to_string(target_unit.id));
        } else if (target_unit.alive &&
                   target_unit.faction == unit.faction) {
          add("target-enemy", i,
              "targets same-faction unit " + std::to_string(target_unit.id));
        }
      }
    }

    // Squad membership, unit -> squad direction.
    if (i < unit_squad_.size() && unit_squad_[i] >= 0) {
      const std::size_t squad =
          static_cast<std::size_t>(unit_squad_[i]);
      if (squad >= squads_.size()) {
        add("squad-exists", i,
            "member of squad " + std::to_string(unit_squad_[i]) +
                " but only " + std::to_string(squads_.size()) +
                " squads exist");
      } else if (i >= unit_member_.size() ||
                 static_cast<std::size_t>(unit_member_[i]) >=
                     squads_[squad].members.size()) {
        add("squad-member-index", i,
            "member index " +
                std::to_string(i < unit_member_.size() ? unit_member_[i]
                                                       : 0u) +
                " outside squad's " +
                std::to_string(squads_[squad].members.size()) +
                " members");
      } else if (squads_[squad].members[unit_member_[i]].unit_index != i) {
        add("squad-backref", i,
            "squad " + std::to_string(unit_squad_[i]) + " member slot " +
                std::to_string(unit_member_[i]) + " points at unit index " +
                std::to_string(
                    squads_[squad].members[unit_member_[i]].unit_index));
      } else if (squads_[squad].faction != unit.faction) {
        add("squad-faction", i,
            "unit faction " +
                std::to_string(static_cast<unsigned>(unit.faction)) +
                " in squad of faction " +
                std::to_string(
                    static_cast<unsigned>(squads_[squad].faction)));
      }
    }
  }

  // ---- squads ----------------------------------------------------------
  for (const Squad& squad : squads_) {
    if (squad.members.empty()) {
      add("squad-members", InvariantViolation::kNoUnit,
          "squad " + std::to_string(squad.id) + " has no members");
    }
    if (squad.commander_index >= world_.units.size()) {
      add("squad-commander-index", InvariantViolation::kNoUnit,
          "squad " + std::to_string(squad.id) + " commander index " +
              std::to_string(squad.commander_index) + " out of range");
    } else if (squad.command_active &&
               !world_.units[squad.commander_index].alive) {
      add("squad-commander-alive", InvariantViolation::kNoUnit,
          "squad " + std::to_string(squad.id) + " holds active command " +
              "under dead unit " +
              std::to_string(world_.units[squad.commander_index].id));
    }
    if (!finite(squad.cohesion) || squad.cohesion < 0.0 ||
        squad.cohesion > 1.0) {
      add("squad-cohesion", InvariantViolation::kNoUnit,
          "squad " + std::to_string(squad.id) + " cohesion " +
              at(squad.cohesion) + " outside [0, 1]");
    }
    if (!finite(squad.centroid_x) || !finite(squad.centroid_y)) {
      add("squad-centroid", InvariantViolation::kNoUnit,
          "squad " + std::to_string(squad.id) + " centroid (" +
              at(squad.centroid_x) + ", " + at(squad.centroid_y) +
              ") is not finite");
    }
    if (squad.player_order_point &&
        (!finite(squad.player_order_x) || !finite(squad.player_order_y))) {
      add("squad-order-point", InvariantViolation::kNoUnit,
          "squad " + std::to_string(squad.id) +
              " point order target is not finite");
    }
    if (squad.contact_count > kMaxSquadContacts) {
      add("squad-contacts", InvariantViolation::kNoUnit,
          "squad " + std::to_string(squad.id) + " contact_count " +
              std::to_string(squad.contact_count) + " above the maximum " +
              std::to_string(kMaxSquadContacts));
    }
    for (const SquadMemberState& member : squad.members) {
      if (member.unit_index >= world_.units.size()) {
        add("squad-member-unit", InvariantViolation::kNoUnit,
            "squad " + std::to_string(squad.id) + " member points at unit " +
                "index " + std::to_string(member.unit_index) +
                " out of range");
        continue;
      }
      // Membership, squad -> unit direction: the member's unit must
      // point back at this squad and slot.
      const std::size_t unit = member.unit_index;
      if (unit >= unit_squad_.size() ||
          static_cast<std::size_t>(unit_squad_[unit]) !=
              static_cast<std::size_t>(squad.id) ||
          unit >= unit_member_.size() ||
          squads_[static_cast<std::size_t>(unit_squad_[unit])]
                 .members[unit_member_[unit]]
                 .unit_index != unit) {
        add("squad-backref-unit", InvariantViolation::kNoUnit,
            "squad " + std::to_string(squad.id) + " lists unit index " +
                std::to_string(unit) + " which does not point back");
      }
      if (static_cast<std::uint8_t>(member.order) >
          static_cast<std::uint8_t>(Order::None)) {
        add("squad-member-order", InvariantViolation::kNoUnit,
            "squad " + std::to_string(squad.id) + " member order " +
                std::to_string(static_cast<unsigned>(member.order)) +
                " has no Order value");
      }
    }
  }

  // ---- in-flight order queue -------------------------------------------
  std::uint64_t previous_deliver = 0;
  for (const PlayerOrder& order : pending_orders_) {
    if (static_cast<std::size_t>(order.faction) >= faction_count_) {
      add("order-faction", InvariantViolation::kNoUnit,
          "pending order faction index " +
              std::to_string(static_cast<unsigned>(order.faction)) +
              " not fielded");
    }
    if (order.squad_id >= squads_.size()) {
      add("order-squad", InvariantViolation::kNoUnit,
          "pending order targets squad " + std::to_string(order.squad_id) +
              ", only " + std::to_string(squads_.size()) + " exist");
    } else if (squads_[order.squad_id].faction != order.faction) {
      add("order-squad-faction", InvariantViolation::kNoUnit,
          "pending order of faction " +
              std::to_string(static_cast<unsigned>(order.faction)) +
              " targets squad " + std::to_string(order.squad_id) +
              " of another faction");
    }
    if (order.issue_tick == 0 || order.deliver_tick < order.issue_tick) {
      add("order-ticks", InvariantViolation::kNoUnit,
          "pending order issue_tick " + std::to_string(order.issue_tick) +
              " / deliver_tick " + std::to_string(order.deliver_tick) +
              " (deliver must be >= issue, issue >= 1)");
    }
    if (order.deliver_tick < previous_deliver) {
      add("order-sorted", InvariantViolation::kNoUnit,
          "pending order queue not sorted by deliver_tick (" +
              std::to_string(order.deliver_tick) + " after " +
              std::to_string(previous_deliver) + ")");
    }
    previous_deliver = order.deliver_tick;
    if (order.has_point && (!finite(order.x) || !finite(order.y))) {
      add("order-point", InvariantViolation::kNoUnit,
          "pending point order target is not finite");
    }
    if (static_cast<std::size_t>(order.type) >= kPlayerOrderTypeCount) {
      add("order-type", InvariantViolation::kNoUnit,
          "pending order type " +
              std::to_string(static_cast<unsigned>(order.type)) +
              " has no PlayerOrderType value");
    }
  }

  // ---- spatial grid contents equal unit positions ----------------------
  // Only when the grid is actually maintained (grid neighbor mode or the
  // utility AI); in brute-force legacy mode it is neither built nor kept
  // current, so there is nothing to verify.
  if (use_grid_) {
    std::vector<std::string> messages;  // empty until a violation fires
    grid_.verify_contents(world_.units, messages);
    for (const std::string& message : messages) {
      add("grid-contents", InvariantViolation::kNoUnit, message);
    }
  }

  // ---- per-faction aggregates -------------------------------------------
  for (std::size_t f = 0; f < faction_count_; ++f) {
    if (!finite(estimate_x_[f]) || !finite(estimate_y_[f])) {
      add("estimate-finite", InvariantViolation::kNoUnit,
          "faction " + std::to_string(f) +
              " enemy estimate is not finite");
    }
    if (!finite(hq_x_[f]) || !finite(hq_y_[f])) {
      add("hq-finite", InvariantViolation::kNoUnit,
          "faction " + std::to_string(f) + " HQ is not finite");
    }
    if (!finite(objective_x_[f]) || !finite(objective_y_[f])) {
      add("objective-finite", InvariantViolation::kNoUnit,
          "faction " + std::to_string(f) + " objective is not finite");
    }
    if (faction_reports_[f].deaths > faction_reports_[f].initial) {
      add("faction-deaths", InvariantViolation::kNoUnit,
          "faction " + std::to_string(f) + " deaths " +
              std::to_string(faction_reports_[f].deaths) + " above initial " +
              std::to_string(faction_reports_[f].initial));
    }
  }

  return out;
}

std::string Simulation::invariant_state_dump(std::size_t index) const {
  std::string out;
  if (index == InvariantViolation::kNoUnit ||
      index >= world_.units.size()) {
    out += "state: tick " + std::to_string(world_.tick_count) + ", " +
           std::to_string(world_.units.size()) + " units, " +
           std::to_string(grid_.tracked_count()) + " tracked, " +
           std::to_string(faction_count_) + " factions, " +
           std::to_string(squads_.size()) + " squads, " +
           std::to_string(pending_orders_.size()) + " pending orders\n";
    return out;
  }
  const Unit& unit = world_.units[index];
  out += "unit " + std::to_string(unit.id) + " (index " +
         std::to_string(index) + "): faction " +
         std::to_string(static_cast<unsigned>(unit.faction)) + ", " +
         (unit.alive ? "alive" : "dead") +
         (unit.routed ? ", routed" : "") + "\n";
  out += "  position (" + at(unit.x) + ", " + at(unit.y) +
         "), health " + at(unit.health) + " / " +
         at(unit.stats.max_health) + ", morale " + at(unit.morale) +
         " state " + std::to_string(unit.morale_state) + "\n";
  out += "  fatigue " + at(unit.fatigue) + ", experience " +
         at(unit.experience) + ", facing " + at(unit.facing) +
         ", cooldown " + std::to_string(unit.cooldown_remaining) + "\n";
  out += "  squad " +
         (index < unit_squad_.size()
              ? std::to_string(unit_squad_[index])
              : std::string("?")) +
         ", target " +
         (index < target_index_.size() && target_index_[index] >= 0
              ? std::to_string(target_index_[index])
              : std::string("none")) +
         "\n";
  return out;
}

// ---- InvariantCorruption: the test seam --------------------------------
// Each function performs exactly the one private write it names; see
// sim/invariants.hpp. Bounds-checked with .at() so a bad test index
// throws instead of corrupting neighbors.

void InvariantCorruption::unit_id(Simulation& s, std::size_t index,
                                  std::uint32_t id) {
  s.world_.units.at(index).id = id;
}

void InvariantCorruption::position(Simulation& s, std::size_t index,
                                   double x, double y) {
  s.world_.units.at(index).x = x;
  s.world_.units.at(index).y = y;
}

void InvariantCorruption::health(Simulation& s, std::size_t index,
                                 double health) {
  s.world_.units.at(index).health = health;
}

void InvariantCorruption::max_health(Simulation& s, std::size_t index,
                                     double max_health) {
  s.world_.units.at(index).stats.max_health = max_health;
}

void InvariantCorruption::morale(Simulation& s, std::size_t index,
                                 double morale) {
  s.world_.units.at(index).morale = morale;
}

void InvariantCorruption::fatigue(Simulation& s, std::size_t index,
                                  double fatigue) {
  s.world_.units.at(index).fatigue = fatigue;
}

void InvariantCorruption::experience(Simulation& s, std::size_t index,
                                     double experience) {
  s.world_.units.at(index).experience = experience;
}

void InvariantCorruption::facing(Simulation& s, std::size_t index,
                                 double facing) {
  s.world_.units.at(index).facing = facing;
}

void InvariantCorruption::faction(Simulation& s, std::size_t index,
                                  Faction faction) {
  s.world_.units.at(index).faction = faction;
}

void InvariantCorruption::target_of(Simulation& s, std::size_t index,
                                    int target) {
  s.target_index_.at(index) = target;
}

void InvariantCorruption::revive(Simulation& s, std::size_t index) {
  s.world_.units.at(index).alive = true;
}

void InvariantCorruption::unit_squad(Simulation& s, std::size_t index,
                                     std::int32_t squad_id) {
  s.unit_squad_.at(index) = squad_id;
}

void InvariantCorruption::squad_member_index(Simulation& s,
                                             std::size_t index,
                                             std::uint32_t member_index) {
  s.unit_member_.at(index) = member_index;
}

void InvariantCorruption::squad_commander(Simulation& s, std::size_t squad,
                                          std::uint32_t commander_index,
                                          bool command_active) {
  s.squads_.at(squad).commander_index = commander_index;
  s.squads_.at(squad).command_active = command_active;
}

void InvariantCorruption::squad_cohesion(Simulation& s, std::size_t squad,
                                         double cohesion) {
  s.squads_.at(squad).cohesion = cohesion;
}

void InvariantCorruption::push_pending_order(
    Simulation& s, const PlayerOrder& order) {
  s.pending_orders_.push_back(order);
}

}  // namespace sim
