#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "sim/morale.hpp"
#include "sim/unit.hpp"

namespace sim {

inline constexpr std::size_t kOrderCount = 4;  // Advance..Retreat (not None)

// The four commander orders (M6). An order is a bounded score term in the
// utility AI — never an override: it nudges action scores, availability rules
// stay untouched. None means "no order in effect" (squadless unit, order
// decayed, or the squad is between commanders).
enum class Order : std::uint8_t {
  Advance = 0,  // push toward the enemy / the faction objective
  Hold = 1,     // stand and fight; the squad keeps its ground
  Regroup = 2,  // rally on the commander; pull the squad back together
  Retreat = 3,  // fall back toward the own side
  None = 4,
};

[[nodiscard]] const char* to_string(Order order) noexcept;

// Player order types (m11b), issued through Simulation::issue_order.
// Each maps onto the commander-order system (Order + extras): MoveTo and
// AttackMove are Advance plus a squad objective point (marched to via a
// squad-specific flow field; MoveTo keeps marching past enemies,
// AttackMove engages normally), Hold and Regroup are the same-named
// commander orders, Retreat optionally carries a point to fall back to,
// and ChangeFormation swaps the squad's formation shape.
enum class PlayerOrderType : std::uint8_t {
  MoveTo = 0,
  AttackMove = 1,
  Hold = 2,
  Retreat = 3,
  Regroup = 4,
  ChangeFormation = 5,
};

inline constexpr std::size_t kPlayerOrderTypeCount = 6;

[[nodiscard]] const char* to_string(PlayerOrderType type) noexcept;
[[nodiscard]] bool parse_player_order_type(std::string_view text,
                                           PlayerOrderType& out) noexcept;

// The three formation shapes (M6). Slot offsets are generated in code from
// the member count and the formation's spacing (data/formations.json); the
// file picks which shapes exist, their spacing and the default shape.
enum class FormationKind : std::uint8_t {
  Line = 0,   // one rank, lateral, filled center-out (commander at the center)
  Column = 1, // single file, front to back (commander at the front)
  Loose = 2,  // square-ish block, rows front to back, center-out per row
};

// One player order in flight to one squad (m11b). Delivery lands at
// deliver_tick; the obedience weight is computed at delivery.
struct PlayerOrder {
  std::uint64_t issue_tick = 0;   // 1-based tick the order was issued
  std::uint64_t deliver_tick = 0; // issue_tick + courier delay
  Faction faction = Faction::Red;
  std::uint32_t squad_id = 0;     // global squad id
  PlayerOrderType type = PlayerOrderType::Hold;
  double x = 0.0;                 // target point (point orders)
  double y = 0.0;
  bool has_point = false;         // MoveTo / AttackMove / Retreat(point)
  bool formation_set = false;     // ChangeFormation target below
  FormationKind formation = FormationKind::Line;
  double formation_spacing = 3.0;
  double obedience = 0.0;         // recorded at delivery
};

// data/formations.json (M6): the defined formations, the default one squads
// use, and the steering weights for members that hold formation.
struct FormationConfig {
  struct Definition {
    std::string id;  // "line" | "column" | "loose"
    FormationKind kind = FormationKind::Line;
    double spacing = 3.0;  // world units between neighbouring slots (> 0)
  };
  std::vector<Definition> formations;  // in file order; ids unique
  std::string default_formation;       // must name one of them

  // Steering weights (all >= 0). A member in formation steers along the
  // weighted sum of separation, cohesion, alignment and slot seeking, then
  // moves min(speed, |steering|) along it. The near-slot pull weights
  // (slot seeking + cohesion) are deliberately small so the proportional
  // move rule stays stable: with a combined pull gain above speed / pull,
  // members overshoot their slots and settle into a saturated two-tick
  // oscillation instead of converging (measured; see docs/squads.md).
  struct Steering {
    double separation = 1.5;   // away from units inside separation_radius
    double cohesion = 0.1;     // toward the squad centroid
    double alignment = 0.05;   // along the squad's mean last move
    double slot_seeking = 0.35; // toward the member's formation slot
    double separation_radius = 3.0;  // separation query radius / min spacing
    // Cohesion metric scale: squad cohesion = clamp(1 - mean distance of
    // alive members to the squad centroid / cohesion_reference, 0, 1).
    double cohesion_reference = 60.0;
  } steering;
};

// data/commanders.json (M6): order rules and thresholds, propagation,
// succession, the commander-death morale shock, and the obedience model.
// Every field is required and validated; there are no silent defaults.
struct CommanderConfig {
  // Order selection from squad state. First match wins, checked in the
  // fixed priority retreat > regroup > hold > advance:
  //   retreat — cohesion < retreat_cohesion_below OR the commander's local
  //             strength ratio < retreat_strength_below OR the squad's mean
  //             morale < retreat_morale_below
  //   regroup — cohesion < regroup_cohesion_below
  //   hold    — enemies visible to the commander AND strength >
  //             hold_strength_above
  //   advance — otherwise (the default order)
  struct Rules {
    double retreat_cohesion_below = 0.15;
    double retreat_strength_below = 0.30;
    double retreat_morale_below = 15.0;
    double regroup_cohesion_below = 0.45;
    double hold_strength_above = 0.65;
  } rules;

  // The bounded order score terms: the bonus added to the matching action
  // scores is obedience * weight, so each term is capped by its weight.
  // Which actions an order boosts (only where available, then clamped to
  // [0, 1]): advance -> Advance + Approach; hold -> Hold + Attack;
  // regroup -> Hold; retreat -> Retreat.
  struct OrderWeights {
    double advance = 0.30;
    double hold = 0.30;
    double regroup = 0.30;
    double retreat = 0.30;
  } order_weights;

  // Order propagation and lifetime. A member inside the commander's
  // leadership radius adopts the commander's current order once
  // (current tick - issue tick) >= ceil(distance * order_delay_per_unit);
  // outside the radius the last received order decays to None after
  // order_decay_ticks consecutive out-of-range ticks. A commander keeps a
  // new order for at least order_min_duration_ticks before re-evaluating
  // (hysteresis, so orders do not flip-flop with squad state).
  double order_delay_per_unit = 0.02;  // ticks of delay per world unit
  std::uint64_t order_decay_ticks = 60;
  std::uint64_t order_min_duration_ticks = 40;

  // Succession: after a commander dies or routs (command is lost at that
  // tick), the squad drifts with no order term until, exactly
  // succession_delay_ticks later, the lowest-id alive, non-routed member
  // takes command (a successor with no recovered former commander simply
  // re-assumes command if eligible). Routed commanders lose command
  // permanently.
  std::uint64_t succession_delay_ticks = 20;

  // Morale shock (M5 morale systems only): a commander's death subtracts
  // commander_death_morale_loss (scaled by the standard personality loss
  // factor) from every squad member inside the dead commander's leadership
  // radius at the moment of death.
  double commander_death_morale_loss = 18.0;

  // Obedience model: obedience = discipline
  //   * clamp(1 - distance/leadership_radius, 0, 1)   [distance factor]
  //   * morale_factor[state]                          [morale state factor]
  //   * (cohesion_floor + (1 - cohesion_floor) * squad_cohesion)
  // clamped to [0, 1]. Falls with distance, rises with discipline and squad
  // cohesion; Confident units obey more, Shaken ones less, routed (Broken)
  // units never score and thus never obey.
  struct Obedience {
    double morale_factor_confident = 1.1;
    double morale_factor_steady = 1.0;
    double morale_factor_worried = 0.85;
    double morale_factor_shaken = 0.6;
    double cohesion_floor = 0.5;
  } obedience;

  // Player orders (m11b). A player order is issued through
  // Simulation::issue_order and travels through the commander system: the
  // delivery delay from the faction HQ to the squad's acting commander is
  // ceil(distance * order_delay_per_unit) — the same courier speed member
  // orders propagate with — plus succession_delay_ticks when command has
  // been lost (the order goes through the successor) or
  // player_orders.leaderless_delay_ticks when the squad has no eligible
  // leader at issue time (delivered at reduced obedience).
  struct PlayerOrders {
    std::uint32_t max_per_tick = 1;           // per faction per tick
    std::uint64_t leaderless_delay_ticks = 40;
    double leaderless_obedience = 0.5;  // multiplier when leaderless
    // MoveTo (as opposed to AttackMove) keeps marching while enemies are
    // visible: this obedience-scaled bias is added to Hold so a moving
    // squad does not stop to fight everything it sees.
    double moveto_hold_bias = 0.10;
    // A player order is a deliberate command, not a contextual nudge.
    // Its score bonus is (commander-order weight) * weight_scale *
    // delivery_strength, where delivery_strength floors at 0.5 (the
    // faction's authority) and reaches 1.0 at full obedience; the bonus
    // is still clamped to [0, 1] per action — a bounded score term,
    // never an availability override. At commander-order scale (1.0) a
    // Hold order cannot stop a healthy aggressive march (Advance ~0.9
    // vs Hold ~0.3 + 0.3; measured), which would make orders cosmetic;
    // 4.0 lets a delivered order decide for most units while the most
    // aggressive, healthy ones can still refuse.
    double weight_scale = 4.0;
  } player_orders;
};

// Parses a formation configuration from JSON text (see
// data/formations.json for the shape). Throws std::invalid_argument with a
// descriptive message for every invalid input — malformed JSON, wrong types,
// missing or unknown fields, unknown or duplicate formation ids, non-positive
// spacing, a default formation that is not defined, negative steering
// weights, or non-positive separation radius / cohesion reference. Never
// silently defaults. Performs no file I/O.
[[nodiscard]] FormationConfig parse_formations(std::string_view json_text);

// Same parsing and validation, reading from any std::istream.
[[nodiscard]] FormationConfig parse_formations(std::istream& stream);

// The built-in default formation configuration, identical to the shipped
// data/formations.json. Static storage; do not modify.
[[nodiscard]] const FormationConfig& default_formation_config() noexcept;

// Parses a commander configuration from JSON text (see
// data/commanders.json for the shape). Throws std::invalid_argument with a
// descriptive message for every invalid input — malformed JSON, wrong types,
// missing or unknown fields, thresholds outside their documented ranges,
// order weights outside [0, 1], non-positive delays, or a negative death
// shock. Never silently defaults. Performs no file I/O.
[[nodiscard]] CommanderConfig parse_commanders(std::string_view json_text);

// Same parsing and validation, reading from any std::istream.
[[nodiscard]] CommanderConfig parse_commanders(std::istream& stream);

// The built-in default commander configuration, identical to the shipped
// data/commanders.json. Static storage; do not modify.
[[nodiscard]] const CommanderConfig& default_commander_config() noexcept;

// One formation slot offset in the squad's local frame: `forward` along the
// squad's advance direction (own spawn band center toward the faction
// objective), `lateral` along the perpendicular axis. Slots are assigned in
// member order: slot 0 (the commander's) is the center of a line, the front
// of a column and the front-center of a loose block.
struct SlotOffset {
  double forward = 0.0;
  double lateral = 0.0;
};

// The slot offset of member `index` (0-based, in member order) in a
// formation of `member_count` members of the given kind with the given
// spacing. Pure and deterministic.
[[nodiscard]] SlotOffset formation_slot(FormationKind kind,
                                        std::size_t member_count,
                                        double spacing,
                                        std::size_t index) noexcept;

// Selects an order from the squad state (see CommanderConfig::Rules for the
// exact conditions and their priority). Pure and deterministic.
[[nodiscard]] Order pick_order(double cohesion, double strength,
                               double squad_morale, bool enemies_visible,
                               const CommanderConfig::Rules& rules) noexcept;

// The obedience weight in [0, 1] (see CommanderConfig::Obedience for the
// formula). Pure and deterministic.
[[nodiscard]] double obedience_weight(
    double discipline, double distance, double leadership_radius,
    MoraleState state, double squad_cohesion,
    const CommanderConfig& config) noexcept;

// The per-action score bonus of an order: obedience * weight on the actions
// the order favors (see CommanderConfig::OrderWeights). Index by Action.
[[nodiscard]] std::array<double, kActionCount> order_score_bonus(
    Order order, double obedience,
    const CommanderConfig::OrderWeights& weights) noexcept;

// Per-faction squad statistics (M6), tracked over the whole battle and
// reported in exports (format 5) and the CLI report.
struct SquadStats {
  std::uint32_t squads = 0;  // squads the faction fielded
  std::uint64_t commander_deaths = 0;  // deaths of acting commanders
  std::uint64_t successions = 0;       // successors that took command
  // Order issuance counts (the initial pick and every change), indexed by
  // Order (Advance..Retreat).
  std::array<std::uint64_t, kOrderCount> order_counts{};
  std::uint64_t drops_from_formation = 0;  // member left its formation slot
};

// One member of one squad: per-member order state (the effective order may
// lag the commander's current order by the propagation delay, or have
// decayed to None out of range / after command loss).
struct SquadMemberState {
  std::uint32_t unit_index = 0;  // index into the world units vector
  Order order = Order::None;
  std::uint32_t out_of_range_ticks = 0;  // consecutive ticks beyond the radius
  bool in_formation = false;  // held formation on the last tick (M6 stats)
};

// M10 squad intel: one shared enemy contact. Members inside the acting
// commander's leadership radius publish the position of the nearest enemy
// they see; other members may read a contact only
// squad_intel_delay_ticks after it was recorded (the same kind of delay
// orders travel with).
struct SquadContact {
  double x = 0.0;
  double y = 0.0;
  std::uint64_t tick = 0;  // 1-based battle tick it was seen on
};

inline constexpr std::size_t kMaxSquadContacts = 8;

// One squad: a fixed member list, its commander and command state, the
// current order and the last computed cohesion/centroid. Member order is
// commander first, then soldiers by ascending unit index; the member's slot
// index is its position in that order.
struct Squad {
  std::uint32_t id = 0;        // global squad id (Red squads, then Blue)
  Faction faction = Faction::Red;
  FormationKind formation = FormationKind::Line;
  double spacing = 3.0;        // from the squad's formation definition
  double leadership_radius = 0.0;  // from the original commander's type
  std::uint32_t commander_index = 0;  // world unit index of the commander
  bool command_active = false;  // the commander currently holds command
  std::uint64_t command_lost_tick = 0;  // 1-based; 0 = never lost
  Order order = Order::None;    // the commander's current order
  std::uint64_t order_issue_tick = 0;  // 1-based tick the order was issued
  // m11b player order state. A delivered player order takes over the
  // squad's order (the commander executes it; the commander AI does not
  // re-evaluate while it is active) and persists until replaced by a
  // later player order, command loss or squad collapse. `has_point`
  // marks point orders (MoveTo / AttackMove / Retreat to a point): the
  // squad's members and commander march along a squad-specific flow
  // field toward (player_order_x, player_order_y) instead of the
  // faction field.
  bool player_order_active = false;
  PlayerOrderType player_order_type = PlayerOrderType::Hold;
  bool player_order_point = false;
  double player_order_x = 0.0;
  double player_order_y = 0.0;
  double cohesion = 0.0;        // last computed, [0, 1]
  double centroid_x = 0.0;      // last computed, alive members
  double centroid_y = 0.0;
  // M10 shared intel: the freshest kMaxSquadContacts contacts, newest
  // first (insertion keeps this order; older ones fall off the end).
  std::array<SquadContact, kMaxSquadContacts> contacts{};
  std::uint32_t contact_count = 0;
  std::vector<SquadMemberState> members;  // commander first, then soldiers
};

}  // namespace sim
