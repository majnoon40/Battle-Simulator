#include "sim/ai.hpp"

#include <algorithm>
#include <cstdint>

#include "sim/unit.hpp"

namespace sim {

const char* to_string(Action action) noexcept {
  switch (action) {
    case Action::Attack:
      return "attack";
    case Action::Approach:
      return "approach";
    case Action::Advance:
      return "advance";
    case Action::Retreat:
      return "retreat";
    case Action::Hold:
      return "hold";
    case Action::Search:
      return "search";
    case Action::Help:
      return "help";
    case Action::Flank:
      return "flank";
    case Action::None:
      return "none";
    case Action::Flee:
      return "flee";
  }
  return "unknown";
}

// The score formulas (weights sum to exactly 1.0 per action; every input is
// clamped to [0, 1] by the caller):
//
//   attack   = 0.25 + 0.20*health + 0.20*strength + 0.15*readiness
//                    + 0.20*bravery                (enemy in range)
//   approach = 0.20 + 0.25*aggression + 0.20*strength + 0.15*health
//                    + 0.20*distance               (enemy visible)
//   advance  = 0.50 + 0.30*aggression + 0.20*health (no enemy visible)
//   retreat  = 0.10 + 0.35*(1-health) + 0.20*(1-strength)
//                    + 0.35*caution                (enemy visible)
//   hold     = 0.15 + 0.30*caution + 0.25*(1-health) + 0.30*(1-readiness)
//
// Rationale: Attack rewards health, local superiority and a ready weapon, and
// bravery makes low-health units keep fighting instead of breaking. Approach
// rises with aggression and with distance (a far enemy cannot be attacked,
// so closing is the productive move). Advance dominates the no-enemy case
// (0.50 base) so armies keep moving, but wounded, cautious units prefer
// Hold. Retreat grows as health and the local strength ratio fall and with
// caution; only the most extreme cowards (caution far above aggression) flee
// a full-health, even duel. All unavailable actions score -1.0.
const char* to_string(SpawnPattern pattern) noexcept {
  switch (pattern) {
    case SpawnPattern::Band:
      return "band";
    case SpawnPattern::Line:
      return "line";
    case SpawnPattern::Columns:
      return "columns";
    case SpawnPattern::Echelon:
      return "echelon";
    case SpawnPattern::Cluster:
      return "cluster";
  }
  return "unknown";
}

ActionScores score_actions(const ScoreInputs& inputs,
                           const TacticsConfig* tactics) {
  ActionScores scores{};
  scores.fill(-1.0);
  const double inverse_health = 1.0 - inputs.health_fraction;
  const double inverse_strength = 1.0 - inputs.strength_ratio;
  const double inverse_readiness = 1.0 - inputs.cooldown_readiness;
  if (inputs.enemy_in_attack_range) {
    scores[static_cast<std::size_t>(Action::Attack)] =
        0.25 + 0.20 * inputs.health_fraction +
        0.20 * inputs.strength_ratio + 0.15 * inputs.cooldown_readiness +
        0.20 * inputs.bravery;
  }
  if (inputs.has_visible_enemy) {
    scores[static_cast<std::size_t>(Action::Approach)] =
        0.20 + 0.25 * inputs.aggression + 0.20 * inputs.strength_ratio +
        0.15 * inputs.health_fraction + 0.20 * inputs.distance_fraction;
    scores[static_cast<std::size_t>(Action::Retreat)] =
        0.10 + 0.35 * inverse_health + 0.20 * inverse_strength +
        0.35 * inputs.caution;
  } else if (tactics != nullptr && inputs.has_fresh_contact) {
    // M10, spec semantics: a unit with no visible enemy heads to its
    // freshest contact (own memory or squad intel); Advance is what it
    // falls back to AFTER forgetting. Search is scored below.
  } else {
    scores[static_cast<std::size_t>(Action::Advance)] =
        0.50 + 0.30 * inputs.aggression + 0.20 * inputs.health_fraction;
  }
  scores[static_cast<std::size_t>(Action::Hold)] =
      0.15 + 0.30 * inputs.caution + 0.25 * inverse_health +
      0.30 * inverse_readiness;

  if (tactics != nullptr) {
    // M10 extensions (formulas in docs/ai.md; weights sum to 1.0).
    if (!inputs.has_visible_enemy && inputs.has_fresh_contact) {
      scores[static_cast<std::size_t>(Action::Search)] = search_score(
          inputs.contact_distance, inputs.sight_radius,
          static_cast<std::uint64_t>(inputs.contact_age),
          tactics->memory.forget_ticks, *tactics);
      // Hold stays available as the careful alternative; Approach,
      // Retreat and Attack need a visible enemy, and Advance is the
      // fallback for after the contact is forgotten.
    }
    if (!inputs.has_visible_enemy && inputs.ally_in_need &&
        inputs.strength_ratio >= tactics->help.strength_ratio_min) {
      scores[static_cast<std::size_t>(Action::Help)] = help_score(
          inputs.ally_health_fraction, inputs.ally_distance,
          tactics->help.radius, *tactics);
    }
    if (inputs.has_visible_enemy && inputs.squad_engaged &&
        inputs.member_may_flank &&
        inputs.local_enemy_ratio >= tactics->flank.outnumber_ratio) {
      scores[static_cast<std::size_t>(Action::Flank)] =
          flank_score(inputs.strength_ratio, *tactics);
    }
    // Bounded pursuit of routed enemies: a score bonus on Approach.
    if (inputs.has_visible_enemy && inputs.target_routed &&
        inputs.pursuit_within_bounds) {
      scores[static_cast<std::size_t>(Action::Approach)] = std::clamp(
          scores[static_cast<std::size_t>(Action::Approach)] +
              pursuit_bonus(true, inputs.unit_speed, *tactics), 0.0, 1.0);
    }
    // Regroup term: a bonus on Advance at low squad cohesion with no
    // enemy visible. m13 item 2 (--ai-avail v2): never lift an
    // unavailable Advance — clamp(-1 + 0, 0, 1) used to turn the
    // engaged placeholder into a pseudo-available 0.0, which the m11b
    // order bonus then amplified into "ordered armies never close to
    // melee" (the m12 audit's root cause). v1 keeps the exact M10
    // clamp for byte-identical replays.
    if (inputs.has_squad &&
        (!inputs.availability_v2 ||
         scores[static_cast<std::size_t>(Action::Advance)] >= 0.0)) {
      scores[static_cast<std::size_t>(Action::Advance)] = std::clamp(
          scores[static_cast<std::size_t>(Action::Advance)] +
              regroup_bonus(inputs.squad_cohesion, inputs.has_visible_enemy,
                            *tactics), 0.0, 1.0);
    }
  }
  // Inputs are clamped by the caller, but clamp once more so a caller bug can
  // never produce a non-finite or out-of-range score.
  for (double& score : scores) {
    if (score >= 0.0) {
      score = std::clamp(score, 0.0, 1.0);
    }
  }
  return scores;
}

Action choose_action(const ActionScores& scores, Action current,
                     double stickiness,
                     bool bound_stickiness) noexcept {
  std::size_t best = static_cast<std::size_t>(Action::Hold);
  double best_score = -1.0;
  // Enum order is the tie-break order, so scanning ascending and requiring a
  // STRICTLY greater score keeps the earlier action on exact ties.
  for (std::size_t a = 0; a < scores.size(); ++a) {
    double score = scores[a];
    if (a == static_cast<std::size_t>(current) && current != Action::None) {
      score += stickiness;
      // m12 audit: bounded order terms can saturate several actions at
      // 1.0; an uncapped stickiness bonus then pins the current action
      // forever (a Hold order kept Hold over Attack). Bound the bonus at
      // the score ceiling so saturated actions tie-break by enum
      // priority (Attack first) instead.
      if (bound_stickiness) {
        score = std::min(score, 1.0);
      }
    }
    if (score > best_score) {
      best_score = score;
      best = a;
    }
  }
  return static_cast<Action>(best);
}

}  // namespace sim
