#include "cli/replay_runtime.hpp"

#include "sim/squads.hpp"

namespace cli {

namespace {

// The CLI's apply-script-order rule (m11b): "all" addresses every squad
// of the faction at issue time; a rejected order warns but never stops
// the run.
void apply_order(sim::Simulation& simulation,
                 const OrderIntervention& order,
                 std::vector<std::string>* warnings) {
  const sim::Faction faction =
      order.red ? sim::Faction::Red : sim::Faction::Blue;
  std::vector<std::uint32_t> squads;
  if (order.squads_all) {
    for (const sim::Squad& squad : simulation.squads()) {
      if (squad.faction == faction) {
        squads.push_back(squad.id);
      }
    }
  } else {
    squads = order.squads;
  }
  if (!simulation.issue_order(faction, squads, order.type, order.x,
                              order.y, order.formation)) {
    if (warnings != nullptr) {
      warnings->push_back(
          "warning: order at tick " + std::to_string(order.tick) + " (" +
          sim::to_string(order.type) +
          ") was rejected (invalid squad, rate limit or disabled orders)");
    }
  }
}

}  // namespace

ReplayDriver::ReplayDriver(
    sim::Simulation& simulation, const std::vector<Intervention>& interventions,
    const std::vector<OrderIntervention>& orders)
    : simulation_(simulation),
      interventions_(interventions),
      orders_(orders) {}

void ReplayDriver::step(std::vector<std::string>* warnings) {
  const std::uint64_t upcoming = simulation_.world().tick_count + 1;
  while (next_intervention_ < interventions_.size() &&
         interventions_[next_intervention_].tick <= upcoming) {
    if (!simulation_.intervene_kill(interventions_[next_intervention_].unit)) {
      if (warnings != nullptr) {
        warnings->push_back(
            "warning: intervention kill: unit " +
            std::to_string(interventions_[next_intervention_].unit) +
            " at tick " +
            std::to_string(interventions_[next_intervention_].tick) +
            " has no effect (unknown or already dead id)");
      }
    }
    ++next_intervention_;
  }
  while (next_order_ < orders_.size() &&
         orders_[next_order_].tick <= upcoming) {
    apply_order(simulation_, orders_[next_order_], warnings);
    ++next_order_;
  }
  simulation_.tick();
}

ReplayDriver::RunResult ReplayDriver::run(std::uint64_t target_tick,
                                          ReplayData* record,
                                          const ReplayData* verify,
                                          std::vector<std::string>* warnings) {
  RunResult result;
  std::size_t next_checkpoint = 0;
  while (!simulation_.finished()) {
    step(warnings);
    const std::uint64_t tick = simulation_.world().tick_count;
    const bool checkpoint = tick % 25 == 0 || simulation_.finished() ||
                            (target_tick != 0 && tick >= target_tick);
    if (!checkpoint) {
      continue;
    }
    const std::string hash = simulation_.state_hash();
    if (record != nullptr) {
      record->hashes.emplace_back(tick, hash);
    }
    if (verify != nullptr) {
      while (next_checkpoint < verify->hashes.size() &&
             verify->hashes[next_checkpoint].first < tick) {
        ++next_checkpoint;  // recorded checkpoints below the re-run tick
      }
      if (next_checkpoint < verify->hashes.size() &&
          verify->hashes[next_checkpoint].first == tick) {
        if (verify->hashes[next_checkpoint].second != hash) {
          result.mismatch = true;
          result.mismatch_tick = tick;
          result.expected = verify->hashes[next_checkpoint].second;
          result.got = hash;
          result.last_tick = tick;
          return result;
        }
        ++next_checkpoint;
        result.checkpoints_verified = next_checkpoint;
      }
    }
    result.last_tick = tick;
    if (target_tick != 0 && tick >= target_tick) {
      break;  // seek: stop here (cost: a full re-run from tick 0)
    }
  }
  result.last_tick = simulation_.world().tick_count;
  return result;
}

Arguments arguments_from_replay(const ReplayData& replay) {
  cli::Arguments effective;
  effective.scenario_path = replay.scenario;
  effective.mod_dir = replay.mod == "-" ? std::string{} : replay.mod;
  effective.seed = static_cast<std::uint32_t>(replay.seed);
  effective.ticks = replay.ticks;
  effective.variance_on = replay.variance == "on";
  effective.variance_explicit = true;
  effective.contact_seek_on = replay.contact_seek == "on";
  effective.flee_clamp_on = replay.flee_clamp == "on";
  effective.squads_on = replay.squads == "on";
  effective.squads_explicit = true;
  effective.grid_mode = replay.neighbor == "grid";
  effective.neighbor_mode_explicit = true;
  effective.swap_sides = replay.swap_sides == "on";
  effective.swap_mirror = replay.swap_sides == "mirror";
  effective.nav_v2 = replay.nav == "v2";
  effective.ai_avail_v2 = replay.ai_avail == "v2";
  effective.end_v2 = replay.end == "v2";
  effective.combat_v2 = replay.combat == "v2";
  effective.orders_on = replay.orders == "on";
  return effective;
}

}  // namespace cli
