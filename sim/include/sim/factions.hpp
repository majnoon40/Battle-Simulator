#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sim {

// Faction identifiers. The legacy game is exactly two factions (M1); the
// values are stable indices used for per-faction statistics arrays. M14
// item 2 extends battles to up to kMaxFactions factions: Red and Blue keep
// their historical indices and west/east spawn bands (every 2-faction
// battle is bit-identical to the legacy runs), extra factions take indices
// 2..7 in scenario file order and spawn in deterministic perimeter sectors.
enum class Faction : std::uint8_t {
  Red = 0,
  Blue = 1,
  Green = 2,
  Yellow = 3,
  Purple = 4,
  Orange = 5,
  Cyan = 6,
  Magenta = 7
};

// The legacy two-faction count. Per-faction state arrays are sized
// kMaxFactions and iterated up to a runtime faction count, so 2-faction
// battles traverse exactly the same slots in the same order as before.
inline constexpr std::size_t kFactionCount = 2;
inline constexpr std::size_t kMaxFactions = 8;
static_assert(kFactionCount <= kMaxFactions);

inline constexpr std::array<Faction, kFactionCount> kAllFactions{
    Faction::Red, Faction::Blue};

[[nodiscard]] const char* to_string(Faction faction) noexcept;

}  // namespace sim
