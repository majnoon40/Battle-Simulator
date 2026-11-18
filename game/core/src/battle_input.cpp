#include "game/battle_input.hpp"

#include <algorithm>

#include "sim/squads.hpp"

namespace game {

bool SquadSelection::contains(std::uint32_t squad_id) const noexcept {
  return std::binary_search(squads.begin(), squads.end(), squad_id);
}

namespace {

// Sorted-unique insert-or-remove: a shift-click toggles membership.
std::vector<std::uint32_t> toggle(std::vector<std::uint32_t> squads,
                                  std::uint32_t squad_id) {
  const auto it = std::lower_bound(squads.begin(), squads.end(), squad_id);
  if (it != squads.end() && *it == squad_id) {
    squads.erase(it);
  } else {
    squads.insert(it, squad_id);
  }
  return squads;
}

}  // namespace

SquadSelection select_squad_click(const SquadSelection& previous,
                                  std::int32_t picked, bool shift) {
  SquadSelection out;
  if (picked < 0) {
    // Clicking nothing clears; shift-clicking nothing keeps the
    // selection (letting the player check the map without losing it).
    out.squads = shift ? previous.squads : std::vector<std::uint32_t>{};
    return out;
  }
  const auto id = static_cast<std::uint32_t>(picked);
  if (!shift) {
    out.squads = {id};
    return out;
  }
  out.squads = toggle(previous.squads, id);
  return out;
}

SquadSelection select_squad_rect(const sim::Simulation& simulation,
                                 const SquadSelection& previous, double x0,
                                 double y0, double x1, double y1, bool shift) {
  const double lo_x = std::min(x0, x1);
  const double hi_x = std::max(x0, x1);
  const double lo_y = std::min(y0, y1);
  const double hi_y = std::max(y0, y1);
  SquadSelection out;
  for (const sim::Squad& squad : simulation.squads()) {
    if (squad.faction != sim::Faction::Red) {
      continue;  // orders are Red-only, so selection is too
    }
    bool inside = false;
    for (const sim::SquadMemberState& member : squad.members) {
      const sim::Unit& unit =
          simulation.world().units[member.unit_index];
      if (unit.alive && unit.x >= lo_x && unit.x <= hi_x && unit.y >= lo_y &&
          unit.y <= hi_y) {
        inside = true;
        break;
      }
    }
    if (inside) {
      out.squads.push_back(squad.id);
    }
  }
  if (shift) {
    out.squads.insert(out.squads.end(), previous.squads.begin(),
                      previous.squads.end());
  }
  std::sort(out.squads.begin(), out.squads.end());
  out.squads.erase(std::unique(out.squads.begin(), out.squads.end()),
                   out.squads.end());
  return out;
}

const char* order_glyph(OrderType type) noexcept {
  switch (type) {
    case OrderType::MoveTo:
      return "M";
    case OrderType::AttackMove:
      return "A";
    case OrderType::Hold:
      return "H";
    case OrderType::Retreat:
      return "R";
    case OrderType::Regroup:
      return "G";
  }
  return "?";
}

OrderIntent order_from_right_click(const SquadSelection& selection,
                                   bool attack_modifier, double wx,
                                   double wy) {
  OrderIntent intent;
  if (selection.empty()) {
    return intent;
  }
  intent.valid = true;
  intent.type = attack_modifier ? OrderType::AttackMove : OrderType::MoveTo;
  intent.squads = selection.squads;
  intent.x = wx;
  intent.y = wy;
  return intent;
}

OrderIntent order_from_hotkey(const SquadSelection& selection,
                              OrderType type) {
  OrderIntent intent;
  if (selection.empty()) {
    return intent;
  }
  intent.valid = true;
  intent.type = type;
  intent.squads = selection.squads;
  return intent;
}

bool apply_order(sim::Simulation& simulation, const OrderIntent& intent) {
  if (!intent.valid) {
    return false;
  }
  sim::PlayerOrderType type = sim::PlayerOrderType::Hold;
  switch (intent.type) {
    case OrderType::MoveTo:
      type = sim::PlayerOrderType::MoveTo;
      break;
    case OrderType::AttackMove:
      type = sim::PlayerOrderType::AttackMove;
      break;
    case OrderType::Hold:
      type = sim::PlayerOrderType::Hold;
      break;
    case OrderType::Retreat:
      type = sim::PlayerOrderType::Retreat;
      break;
    case OrderType::Regroup:
      type = sim::PlayerOrderType::Regroup;
      break;
  }
  return simulation.issue_order(sim::Faction::Red, intent.squads, type,
                                intent.x, intent.y);
}

MinimapGeometry minimap_geometry(int screen_width, int screen_height,
                                 double world_width, double world_height,
                                 int size) {
  MinimapGeometry geometry;
  geometry.size = size;
  geometry.x = 8;  // bottom-left, 8 px margin (HUD top-left, inspector
                   // top-right, squad panel bottom-right)
  geometry.y = screen_height - size - 8;
  geometry.world_w = world_width;
  geometry.world_h = world_height;
  if (geometry.y < 8) {
    geometry.y = 8;  // tiny windows: clamp instead of vanishing
  }
  static_cast<void>(screen_width);
  return geometry;
}

double minimap_x(const MinimapGeometry& geometry, double world_x) noexcept {
  if (geometry.world_w <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(geometry.x) +
         world_x / geometry.world_w * static_cast<double>(geometry.size);
}

double minimap_y(const MinimapGeometry& geometry, double world_y) noexcept {
  if (geometry.world_h <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(geometry.y) +
         world_y / geometry.world_h * static_cast<double>(geometry.size);
}

double world_x_from_minimap(const MinimapGeometry& geometry,
                            double map_x) noexcept {
  if (geometry.size <= 0) {
    return 0.0;
  }
  const double local = map_x - static_cast<double>(geometry.x);
  const double clamped =
      std::clamp(local, 0.0, static_cast<double>(geometry.size));
  return clamped / static_cast<double>(geometry.size) * geometry.world_w;
}

double world_y_from_minimap(const MinimapGeometry& geometry,
                            double map_y) noexcept {
  if (geometry.size <= 0) {
    return 0.0;
  }
  const double local = map_y - static_cast<double>(geometry.y);
  const double clamped =
      std::clamp(local, 0.0, static_cast<double>(geometry.size));
  return clamped / static_cast<double>(geometry.size) * geometry.world_h;
}

bool minimap_covers(const MinimapGeometry& geometry, int screen_x,
                    int screen_y) noexcept {
  return screen_x >= geometry.x && screen_x < geometry.x + geometry.size &&
         screen_y >= geometry.y && screen_y < geometry.y + geometry.size;
}

}  // namespace game
