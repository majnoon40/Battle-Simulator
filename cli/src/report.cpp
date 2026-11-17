#include "cli/report.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <map>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "sim/version.hpp"

namespace cli {
namespace {

using sim::Event;
using sim::EventType;

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);

// --- shared measured data -------------------------------------------------

struct Measured {
  // Outcome.
  sim::BattleResult result;
  // Per faction: initial / deaths / survivors / actives / routs /
  // recoveries / first rout tick.
  // Timeline-derived morale: first / min / last sample values + ticks.
  bool has_morale_samples = false;
  double morale_first[2] = {0, 0};
  double morale_min[2] = {100, 100};
  double morale_last[2] = {0, 0};
  std::uint64_t morale_first_tick = 0;
  std::uint64_t morale_min_tick = 0;
  std::uint64_t morale_last_tick = 0;
  // Event-derived squad facts, keyed by squad id.
  std::map<std::uint32_t, std::uint64_t> squad_kills;
  std::map<std::uint32_t, std::uint64_t> squad_losses;
  std::map<std::uint32_t, std::uint64_t> squad_orders;
  std::map<std::uint32_t, std::uint32_t> squad_last_commander;
  std::map<std::uint32_t, std::uint64_t> squad_commander_lost_tick;
  // Turning points.
  bool has_kill_ratio = false;
  std::uint64_t kill_ratio_tick = 0;
  std::size_t kill_ratio_lead = 0;
  std::size_t kill_ratio_trail = 0;
  std::size_t kill_ratio_faction = 0;
  bool has_commander_death = false;
  std::uint64_t commander_death_tick = 0;
  std::uint32_t commander_death_id = 0;
  std::uint32_t commander_death_squad = 0;
  bool has_mass_rout = false;
  std::uint64_t mass_rout_tick = 0;
  std::size_t mass_rout_count = 0;
  std::size_t mass_rout_faction = 0;
  std::uint64_t max_rout_wave = 0;
  std::uint64_t max_rout_wave_tick = 0;
  std::size_t max_rout_wave_faction = 0;
};

[[nodiscard]] std::size_t count_leq(const std::vector<std::uint64_t>& ticks,
                                    std::uint64_t bound) {
  return static_cast<std::size_t>(
      std::upper_bound(ticks.begin(), ticks.end(), bound) - ticks.begin());
}

Measured measure(const sim::Simulation& simulation,
                 const sim::EventRecorder* events) {
  Measured out;
  out.result = simulation.result();

  // Morale timeline (first / min / last sample).
  const std::vector<sim::TimelineRow>& rows = simulation.timeline();
  if (!rows.empty()) {
    out.has_morale_samples = true;
    out.morale_first_tick = rows.front().tick;
    out.morale_last_tick = rows.back().tick;
    for (std::size_t f = 0; f < 2; ++f) {
      bool first = true;
      for (const sim::TimelineRow& row : rows) {
        const sim::FactionTimeline& side = row.factions[f];
        if (side.morale_n == 0) {
          continue;
        }
        const double value = side.morale_sum / side.morale_n;
        if (first) {
          out.morale_first[f] = value;
          first = false;
        }
        if (value < out.morale_min[f]) {
          out.morale_min[f] = value;
          out.morale_min_tick = row.tick;
        }
        out.morale_last[f] = value;
      }
    }
  }

  if (events == nullptr) {
    return out;
  }

  // id -> unit index (for attributing kills to squads).
  std::unordered_map<std::uint32_t, std::size_t> id_to_index;
  for (std::size_t i = 0; i < simulation.world().units.size(); ++i) {
    id_to_index[simulation.world().units[i].id] = i;
  }
  const auto squad_of_id = [&](std::uint32_t id) -> int {
    const auto it = id_to_index.find(id);
    if (it == id_to_index.end()) {
      return -1;
    }
    return simulation.squad_of(it->second);
  };

  std::vector<std::uint64_t> death_ticks[2];
  std::vector<std::uint64_t> rout_ticks[2];
  std::uint64_t rout_window[2] = {0, 0};  // running 20-tick window count

  for (const Event& event : events->events()) {
    switch (event.type) {
      case EventType::Death: {
        const auto victim = id_to_index.find(event.a);
        if (victim != id_to_index.end()) {
          death_ticks[static_cast<std::size_t>(
                          simulation.world().units[victim->second].faction)]
              .push_back(event.tick);
        }
        if (const int squad = squad_of_id(event.a); squad >= 0) {
          out.squad_losses[static_cast<std::uint32_t>(squad)] += 1;
        }
        if (event.b != 0) {
          if (const int squad = squad_of_id(event.b); squad >= 0) {
            out.squad_kills[static_cast<std::uint32_t>(squad)] += 1;
          }
        }
        break;
      }
      case EventType::Rout: {
        const auto unit_it = id_to_index.find(event.a);
        if (unit_it == id_to_index.end()) {
          break;
        }
        const std::size_t faction = static_cast<std::size_t>(
            simulation.world().units[unit_it->second].faction);
        rout_ticks[faction].push_back(event.tick);
        // 20-tick window count ending at this event's tick.
        const std::size_t in_window =
            count_leq(rout_ticks[faction], event.tick) -
            count_leq(rout_ticks[faction],
                      event.tick < 20 ? 0 : event.tick - 20);
        if (in_window > out.max_rout_wave) {
          out.max_rout_wave = in_window;
          out.max_rout_wave_tick = event.tick;
          out.max_rout_wave_faction = faction;
        }
        const std::size_t initial =
            out.result.factions[faction].initial;
        if (!out.has_mass_rout &&
            in_window >= (initial + 9) / 10) {  // ceil(10%)
          out.has_mass_rout = true;
          out.mass_rout_tick = event.tick;
          out.mass_rout_count = in_window;
          out.mass_rout_faction = faction;
        }
        break;
      }
      case EventType::CommanderDeath:
        if (!out.has_commander_death) {
          out.has_commander_death = true;
          out.commander_death_tick = event.tick;
          out.commander_death_id = event.a;
          out.commander_death_squad = event.b;
        }
        out.squad_commander_lost_tick[event.b] = event.tick;
        break;
      case EventType::Succession:
        out.squad_last_commander[event.b] = event.a;
        break;
      case EventType::OrderChange:
        out.squad_orders[event.a] += 1;
        break;
      default:
        break;
    }
  }

  // Turning point 1: the first tick where one side's kills in the last
  // 50 ticks exceed twice the other side's (with at least 3 kills, so a
  // single early kill cannot trip the rule).
  std::vector<std::uint64_t> ticks;
  for (int f = 0; f < 2; ++f) {
    ticks.insert(ticks.end(), death_ticks[f].begin(), death_ticks[f].end());
  }
  std::sort(ticks.begin(), ticks.end());
  ticks.erase(std::unique(ticks.begin(), ticks.end()), ticks.end());
  for (std::uint64_t t : ticks) {
    const std::size_t red = count_leq(death_ticks[kRed], t) -
                            count_leq(death_ticks[kRed],
                                      t < 50 ? 0 : t - 50);
    const std::size_t blue = count_leq(death_ticks[kBlue], t) -
                             count_leq(death_ticks[kBlue],
                                       t < 50 ? 0 : t - 50);
    if (red >= 3 && red > 2 * blue) {
      out.has_kill_ratio = true;
      out.kill_ratio_tick = t;
      out.kill_ratio_lead = red;
      out.kill_ratio_trail = blue;
      out.kill_ratio_faction = kRed;
      break;
    }
    if (blue >= 3 && blue > 2 * red) {
      out.has_kill_ratio = true;
      out.kill_ratio_tick = t;
      out.kill_ratio_lead = blue;
      out.kill_ratio_trail = red;
      out.kill_ratio_faction = kBlue;
      break;
    }
  }
  return out;
}

[[nodiscard]] const char* faction_name(std::size_t faction) {
  return faction == kRed ? "Red" : "Blue";
}

}  // namespace

// --------------------------------------------------------------------------
// Markdown report.
// --------------------------------------------------------------------------
std::string generate_report_markdown(const sim::Simulation& simulation,
                                     const sim::EventRecorder* events,
                                     const sim::UnitTypeTable& types,
                                     const ReportMeta& meta) {
  const Measured m = measure(simulation, events);
  const sim::BattleResult& r = m.result;
  std::ostringstream out;
  out.imbue(std::locale::classic());

  out << "# Battle report\n\n";
  out << "Scenario `" << meta.scenario << "`, seed " << meta.seed
      << ", sim version " << sim::kSimVersion << ".\n\n";

  // Outcome.
  out << "## Outcome\n\n";
  out << "- Ticks elapsed: " << r.ticks_elapsed << ".\n";
  out << "- Termination: " << sim::to_string(r.reason) << ", winner: "
      << (r.has_winner ? faction_name(static_cast<std::size_t>(r.winner))
                       : "none (draw)")
      << ".\n";
  out << "- First contact: tick "
      << (r.first_contact_tick > 0
              ? std::to_string(r.first_contact_tick)
              : "never (no unit saw an enemy)")
      << ".\n";
  for (std::size_t f = 0; f < 2; ++f) {
    const sim::FactionReport& side = r.factions[f];
    out << "- " << faction_name(f) << ": " << side.initial
        << " units, " << side.deaths << " deaths, " << side.survivors
        << " survivors, " << side.active << " still fighting at the end.\n";
  }

  // Casualties by type.
  out << "\n## Casualties by unit type\n\n";
  out << "| faction | type | spawned | kills | deaths | damage dealt |\n";
  out << "| --- | --- | --- | --- | --- | --- |\n";
  for (std::size_t f = 0; f < 2; ++f) {
    for (const sim::TypeReport& type : r.factions[f].by_type) {
      out << "| " << faction_name(f) << " | " << type.type_id << " | "
          << type.spawned << " | " << type.kills << " | "
          << type.spawned - type.survivors << " | "
          << static_cast<unsigned long long>(type.damage_dealt) << " |\n";
    }
  }

  // Morale.
  out << "\n## Morale timeline\n\n";
  if (m.has_morale_samples) {
    for (std::size_t f = 0; f < 2; ++f) {
      out << "- " << faction_name(f) << " average morale: "
          << m.morale_first[f] << " at tick " << m.morale_first_tick
          << ", minimum " << m.morale_min[f] << ", " << m.morale_last[f]
          << " at the final sample (tick " << m.morale_last_tick << ").\n";
    }
  } else {
    out << "- No timeline samples were recorded for this battle.\n";
  }

  // Combat depth (m13 item 3; the counters are all zero under --combat
  // v1 and the section is omitted).
  const std::uint64_t combat_hits =
      r.factions[0].combat.flank_hits + r.factions[0].combat.rear_hits +
      r.factions[0].combat.charge_hits + r.factions[1].combat.flank_hits +
      r.factions[1].combat.rear_hits + r.factions[1].combat.charge_hits;
  if (combat_hits > 0) {
    out << "\n## Combat depth\n\n";
    for (std::size_t f = 0; f < 2; ++f) {
      const sim::CombatStats& stats = r.factions[f].combat;
      out << "- " << faction_name(f) << ": " << stats.flank_hits
          << " flank hits, " << stats.rear_hits << " rear hits, "
          << stats.charge_hits << " charge hits, "
          << stats.stance_hits_taken << " hits absorbed in stance\n";
    }
    out << "\n";
  }

  // Routing.
  out << "\n## Routing\n\n";
  for (std::size_t f = 0; f < 2; ++f) {
    const sim::RoutingStats& stats = r.factions[f].routing;
    out << "- " << faction_name(f) << ": " << stats.total_routs
        << " routs, " << stats.recoveries << " recoveries";
    if (stats.first_rout_tick != 0) {
      out << ", first rout at tick " << stats.first_rout_tick;
    }
    out << ".\n";
  }
  if (m.max_rout_wave > 0) {
    out << "- Largest rout wave: " << m.max_rout_wave << " "
        << faction_name(m.max_rout_wave_faction)
        << " routs within 20 ticks ending at tick " << m.max_rout_wave_tick
        << ".\n";
  } else if (events != nullptr) {
    out << "- No routs were recorded.\n";
  }

  // Squads.
  const std::vector<sim::Squad>& squads = simulation.squads();
  if (!squads.empty()) {
    out << "\n## Squad performance (ranked by kills)\n\n";
    struct Row {
      std::uint32_t id;
      const char* faction = "";
      std::size_t size = 0;
      std::size_t alive = 0;
      std::uint64_t kills = 0;
      std::uint64_t losses = 0;
      double cohesion = 0.0;
      std::uint64_t orders = 0;
      std::string commander;
    };
    std::vector<Row> rows;
    for (const sim::Squad& squad : squads) {
      Row row;
      row.id = squad.id;
      row.faction = faction_name(static_cast<std::size_t>(squad.faction));
      row.size = squad.members.size();
      for (const sim::SquadMemberState& member : squad.members) {
        if (simulation.world().units[member.unit_index].alive) {
          ++row.alive;
        }
      }
      row.kills = m.squad_kills.count(squad.id) != 0
                      ? m.squad_kills.at(squad.id)
                      : 0;
      row.losses = m.squad_losses.count(squad.id) != 0
                       ? m.squad_losses.at(squad.id)
                       : 0;
      row.cohesion = squad.cohesion;
      row.orders = m.squad_orders.count(squad.id) != 0
                       ? m.squad_orders.at(squad.id)
                       : 0;
      if (squad.command_active) {
        row.commander = "unit " + std::to_string(
                                        simulation.world()
                                            .units[squad.commander_index]
                                            .id) +
                        " (in command)";
      } else {
        row.commander = "lost";
        if (m.squad_commander_lost_tick.count(squad.id) != 0) {
          row.commander += " at tick " +
                           std::to_string(
                               m.squad_commander_lost_tick.at(squad.id));
        }
        if (m.squad_last_commander.count(squad.id) != 0) {
          row.commander += ", last commander unit " +
                           std::to_string(
                               m.squad_last_commander.at(squad.id));
        }
      }
      rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) {
                return a.kills > b.kills || (a.kills == b.kills && a.id < b.id);
              });
    out << "| squad | faction | members | alive | kills | losses | final "
           "cohesion | orders issued | commander |\n";
    out << "| --- | --- | --- | --- | --- | --- | --- | --- | --- |\n";
    for (const Row& row : rows) {
      out << "| " << row.id << " | " << row.faction << " | " << row.size
          << " | " << row.alive << " | " << row.kills << " | " << row.losses
          << " | " << row.cohesion << " | " << row.orders << " | "
          << row.commander << " |\n";
    }
    const std::size_t total_kills = static_cast<std::size_t>(
        std::accumulate(rows.begin(), rows.end(), std::uint64_t{0},
                        [](std::uint64_t sum, const Row& row) {
                          return sum + row.kills;
                        }));
    out << "\nSquad kills sum to " << total_kills
        << " (recorded deaths with a killer: "
        << (events != nullptr ? events->count(EventType::Death) : 0)
        << " events).\n";
  }

  // Commanders.
  if (events != nullptr &&
      (events->count(EventType::CommanderDeath) > 0 ||
       events->count(EventType::Succession) > 0)) {
    out << "\n## Commander events\n\n";
    if (m.has_commander_death) {
      out << "- First commander death: unit " << m.commander_death_id
          << " (squad " << m.commander_death_squad << ") at tick "
          << m.commander_death_tick << ".\n";
    }
    out << "- Commander deaths: "
        << events->count(EventType::CommanderDeath)
        << "; successions: " << events->count(EventType::Succession)
        << ".\n";
  }

  // Player orders (m11b): printed only when at least one was issued.
  {
    const sim::Simulation::PlayerOrderReport& red =
        simulation.player_order_report(sim::Faction::Red);
    const sim::Simulation::PlayerOrderReport& blue =
        simulation.player_order_report(sim::Faction::Blue);
    if (red.issued > 0 || blue.issued > 0) {
      out << "\n## Player orders\n\n";
      for (const auto& [faction, report] :
           std::array<std::pair<const char*,
                                const sim::Simulation::PlayerOrderReport*>,
                      2>{{{"Red", &red}, {"Blue", &blue}}}) {
        if (report->issued == 0) {
          continue;
        }
        out << "- " << faction << ": " << report->issued << " issued, "
            << report->delivered << " delivered, " << report->rejected
            << " rejected, " << report->dropped << " dropped";
        if (report->delivered > 0) {
          out << "; mean delivery delay "
              << static_cast<double>(report->delay_ticks_sum) /
                     static_cast<double>(report->delivered)
              << " ticks, mean obedience "
              << report->obedience_sum /
                     static_cast<double>(report->delivered);
        }
        out << ".\n";
      }
    }
  }

  // Turning points.
  out << "\n## Turning points\n\n";
  if (m.has_kill_ratio) {
    out << "- Kill ratio: from tick " << (m.kill_ratio_tick < 50
                                                ? 1
                                                : m.kill_ratio_tick - 49)
        << " to tick " << m.kill_ratio_tick << ", "
        << faction_name(m.kill_ratio_faction) << " killed "
        << m.kill_ratio_lead << " for " << m.kill_ratio_trail
        << " lost (more than 2:1 with at least 3 kills in the window).\n";
  } else {
    out << "- Kill ratio: no 50-tick window exceeded a 2:1 kill ratio "
           "(with at least 3 kills).\n";
  }
  if (m.has_commander_death) {
    out << "- First commander death at tick " << m.commander_death_tick
        << " (unit " << m.commander_death_id << ").\n";
  } else {
    out << "- No commander died.\n";
  }
  if (m.has_mass_rout) {
    out << "- First mass rout at tick " << m.mass_rout_tick << ": "
        << m.mass_rout_count << " " << faction_name(m.mass_rout_faction)
        << " routs within 20 ticks (at least 10% of its "
        << r.factions[m.mass_rout_faction].initial << " units).\n";
  } else {
    out << "- No 20-tick window reached a 10% rout wave.\n";
  }
  return out.str();
}

// --------------------------------------------------------------------------
// JSON report.
// --------------------------------------------------------------------------
std::string generate_report_json(const sim::Simulation& simulation,
                                 const sim::EventRecorder* events,
                                 const sim::UnitTypeTable& types,
                                 const ReportMeta& meta) {
  const Measured m = measure(simulation, events);
  const sim::BattleResult& r = m.result;
  std::ostringstream out;
  out.imbue(std::locale::classic());
  auto q = [](const std::string& s) { return "\"" + s + "\""; };
  auto num = [](double v) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << v;
    return s.str();
  };

  out << "{\n  \"sim_version\": " << q(sim::kSimVersion) << ",\n";
  out << "  \"scenario\": " << q(meta.scenario) << ",\n";
  out << "  \"seed\": " << meta.seed << ",\n";
  out << "  \"outcome\": {\n    \"ticks\": " << r.ticks_elapsed
      << ",\n    \"reason\": " << q(sim::to_string(r.reason))
      << ",\n    \"winner\": "
      << (r.has_winner
              ? q(faction_name(static_cast<std::size_t>(r.winner)))
              : q("draw"))
      << ",\n    \"first_contact_tick\": " << r.first_contact_tick
      << ",\n    \"units_stuck_over_100\": " << r.units_stuck_over_100
      << ",\n    \"longest_stuck_stretch\": " << r.longest_stuck_stretch
      << "\n  },\n";
  out << "  \"factions\": [\n";
  for (std::size_t f = 0; f < 2; ++f) {
    const sim::FactionReport& side = r.factions[f];
    out << "    {\"name\": " << q(faction_name(f)) << ", \"initial\": "
        << side.initial << ", \"deaths\": " << side.deaths
        << ", \"survivors\": " << side.survivors << ", \"active\": "
        << side.active << ", \"routs\": " << side.routing.total_routs
        << ", \"recoveries\": " << side.routing.recoveries
        << ", \"first_rout_tick\": " << side.routing.first_rout_tick
        << ", \"by_type\": [";
    for (std::size_t t = 0; t < side.by_type.size(); ++t) {
      const sim::TypeReport& type = side.by_type[t];
      out << (t > 0 ? ", " : "") << "{\"type\": " << q(type.type_id)
          << ", \"spawned\": " << type.spawned << ", \"kills\": "
          << type.kills << ", \"damage_dealt\": " << type.damage_dealt
          << ", \"survivors\": " << type.survivors << "}";
    }
    out << "]";
    if (m.has_morale_samples) {
      out << ", \"morale_first\": " << num(m.morale_first[f])
          << ", \"morale_min\": " << num(m.morale_min[f])
          << ", \"morale_last\": " << num(m.morale_last[f]);
    }
    // m11b: player-order statistics, only when that faction issued.
    const sim::Simulation::PlayerOrderReport& player =
        simulation.player_order_report(static_cast<sim::Faction>(f));
    if (player.issued > 0) {
      out << ", \"player_orders\": {\"issued\": " << player.issued
          << ", \"delivered\": " << player.delivered
          << ", \"rejected\": " << player.rejected
          << ", \"dropped\": " << player.dropped
          << ", \"mean_delivery_delay_ticks\": "
          << num(player.delivered > 0
                     ? static_cast<double>(player.delay_ticks_sum) /
                           static_cast<double>(player.delivered)
                     : 0.0)
          << ", \"mean_obedience\": "
          << num(player.delivered > 0
                     ? player.obedience_sum / static_cast<double>(player.delivered)
                     : 0.0)
          << "}";
    }
    out << "}" << (f == 0 ? ",\n" : "\n");
  }
  out << "  ],\n";

  // Squad table.
  const std::vector<sim::Squad>& squads = simulation.squads();
  out << "  \"squads\": [";
  bool first = true;
  for (const sim::Squad& squad : squads) {
    std::size_t alive = 0;
    for (const sim::SquadMemberState& member : squad.members) {
      if (simulation.world().units[member.unit_index].alive) {
        ++alive;
      }
    }
    out << (first ? "" : ", ") << "{\"id\": " << squad.id << ", \"faction\": "
        << q(faction_name(static_cast<std::size_t>(squad.faction)))
        << ", \"members\": " << squad.members.size() << ", \"alive\": "
        << alive << ", \"kills\": "
        << (m.squad_kills.count(squad.id) != 0 ? m.squad_kills.at(squad.id)
                                               : 0)
        << ", \"losses\": "
        << (m.squad_losses.count(squad.id) != 0
                ? m.squad_losses.at(squad.id)
                : 0)
        << ", \"cohesion\": " << num(squad.cohesion) << ", \"orders\": "
        << (m.squad_orders.count(squad.id) != 0 ? m.squad_orders.at(squad.id)
                                                : 0)
        << ", \"command_active\": "
        << (squad.command_active ? "true" : "false") << "}";
    first = false;
  }
  out << "],\n";

  out << "  \"turning_points\": {\n";
  out << "    \"kill_ratio\": ";
  if (m.has_kill_ratio) {
    out << "{\"tick\": " << m.kill_ratio_tick << ", \"faction\": "
        << q(faction_name(m.kill_ratio_faction)) << ", \"kills\": "
        << m.kill_ratio_lead << ", \"losses\": " << m.kill_ratio_trail
        << "}";
  } else {
    out << "null";
  }
  out << ",\n    \"first_commander_death\": ";
  if (m.has_commander_death) {
    out << "{\"tick\": " << m.commander_death_tick << ", \"unit\": "
        << m.commander_death_id << ", \"squad\": " << m.commander_death_squad
        << "}";
  } else {
    out << "null";
  }
  out << ",\n    \"first_mass_rout\": ";
  if (m.has_mass_rout) {
    out << "{\"tick\": " << m.mass_rout_tick << ", \"faction\": "
        << q(faction_name(m.mass_rout_faction)) << ", \"routs\": "
        << m.mass_rout_count << "}";
  } else {
    out << "null";
  }
  out << "\n  }\n}\n";
  return out.str();
}

// --------------------------------------------------------------------------
// Timeline CSV.
// --------------------------------------------------------------------------
std::string generate_timeline_csv(const sim::Simulation& simulation,
                                  const sim::UnitTypeTable& types) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "tick";
  for (const char* faction : {"red", "blue"}) {
    out << "," << faction << "_alive," << faction << "_routed," << faction
        << "_dead," << faction << "_avg_morale," << faction
        << "_engagements";
  }
  for (const char* faction : {"red", "blue"}) {
    for (const sim::UnitType& type : types.types) {
      out << "," << faction << "_" << type.id << "_alive";
    }
  }
  out << "\n";
  for (const sim::TimelineRow& row : simulation.timeline()) {
    out << row.tick;
    for (std::size_t f = 0; f < 2; ++f) {
      const sim::FactionTimeline& side = row.factions[f];
      out << "," << side.alive << "," << side.routed << "," << side.dead
          << ",";
      if (side.morale_n > 0) {
        out << (side.morale_sum / side.morale_n);
      }
      out << "," << side.engagements;
    }
    for (std::size_t f = 0; f < 2; ++f) {
      const std::vector<std::uint32_t>& counts =
          row.factions[f].alive_by_type;
      for (std::size_t t = 0; t < types.types.size(); ++t) {
        out << "," << (t < counts.size() ? counts[t] : 0);
      }
    }
    out << "\n";
  }
  return out.str();
}

}  // namespace cli
