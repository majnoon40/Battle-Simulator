#include "cli/trace.hpp"

#include <cmath>
#include <cstdint>

#include "sim/ai.hpp"
#include "sim/factions.hpp"
#include "sim/unit.hpp"

namespace cli {
namespace {

// One faction's trace object. Uses printf-style formatting for stable,
// locale-independent numbers (same convention as the sim's JSON export).
void append_faction(std::string& out, const char* name,
                    const sim::Simulation& simulation, sim::Faction faction) {
  const sim::World& world = simulation.world();
  std::uint32_t alive = 0;
  double sum_x = 0.0;
  double sum_y = 0.0;
  double sum_moved = 0.0;
  std::uint32_t still = 0;
  std::uint32_t advancing = 0;
  constexpr int kBuckets = 20;
  std::uint32_t hist[kBuckets] = {};
  const double width = world.width;
  for (const sim::Unit& unit : world.units) {
    if (!unit.alive || unit.faction != faction) {
      continue;
    }
    ++alive;
    sum_x += unit.x;
    sum_y += unit.y;
    const double moved =
        std::sqrt(unit.last_move_x * unit.last_move_x +
                  unit.last_move_y * unit.last_move_y);
    sum_moved += moved;
    if (moved < 0.05) {  // the engine's stuck epsilon (kStuckEpsilon)
      ++still;
    }
    if (static_cast<sim::Action>(unit.last_action) == sim::Action::Advance) {
      ++advancing;
    }
    int bucket = static_cast<int>(unit.x / width * kBuckets);
    if (bucket < 0) {
      bucket = 0;
    }
    if (bucket >= kBuckets) {
      bucket = kBuckets - 1;
    }
    ++hist[bucket];
  }
  char buf[64];
  out += "\"";
  out += name;
  out += "\":{";
  std::snprintf(buf, sizeof buf, "\"alive\":%u", alive);
  out += buf;
  if (alive > 0) {
    std::snprintf(buf, sizeof buf, ",\"cx\":%.2f,\"cy\":%.2f",
                  sum_x / alive, sum_y / alive);
    out += buf;
    std::snprintf(buf, sizeof buf, ",\"moved\":%.4f", sum_moved / alive);
    out += buf;
    std::snprintf(buf, sizeof buf, ",\"still\":%.4f",
                  static_cast<double>(still) / alive);
    out += buf;
    std::snprintf(buf, sizeof buf, ",\"advance\":%.4f",
                  static_cast<double>(advancing) / alive);
    out += buf;
  }
  out += ",\"xhist\":[";
  for (int b = 0; b < kBuckets; ++b) {
    if (b > 0) {
      out += ',';
    }
    std::snprintf(buf, sizeof buf, "%u", hist[b]);
    out += buf;
  }
  out += "]}";
}

}  // namespace

std::string trace_line(const sim::Simulation& simulation) {
  const sim::World& world = simulation.world();
  std::string out;
  out.reserve(256);
  char buf[32];
  std::snprintf(buf, sizeof buf, "{\"tick\":%llu,",
                static_cast<unsigned long long>(world.tick_count));
  out += buf;
  append_faction(out, "red", simulation, sim::Faction::Red);
  out += ',';
  append_faction(out, "blue", simulation, sim::Faction::Blue);
  out += "}\n";
  return out;
}

}  // namespace cli
