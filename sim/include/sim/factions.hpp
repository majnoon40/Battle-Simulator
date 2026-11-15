#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sim {

// Faction identifiers. M1 has exactly two factions; the values are stable
// indices used for per-faction statistics arrays.
enum class Faction : std::uint8_t { Red = 0, Blue = 1 };

inline constexpr std::size_t kFactionCount = 2;
inline constexpr std::array<Faction, kFactionCount> kAllFactions{
    Faction::Red, Faction::Blue};

[[nodiscard]] const char* to_string(Faction faction) noexcept;

}  // namespace sim
