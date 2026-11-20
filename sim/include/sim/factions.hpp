#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace sim {

// Faction identifiers. The legacy game is exactly two factions (M1); the
// values are stable indices used for per-faction statistics arrays. M14
// item 2 extends battles to up to kMaxFactions factions: Red and Blue keep
// their historical indices and west/east spawn bands (every 2-faction
// battle is bit-identical to the legacy runs), extra factions take their
// fixed indices 2..7 (Green..Magenta) and spawn in deterministic perimeter
// sectors.
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

// Pi as the nearest double (identical to glibc's M_PI, which MSVC does
// not define — multi-faction sector math uses this constant directly).
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr std::size_t kMaxFactions = 8;
static_assert(kFactionCount <= kMaxFactions);

inline constexpr std::array<Faction, kFactionCount> kAllFactions{
    Faction::Red, Faction::Blue};

// The canonical scenario/JSON key of every faction, index-aligned with the
// enum values ("red" .. "magenta"). The name -> index mapping is fixed, so
// a scenario's faction indices never depend on file order.
inline constexpr std::array<std::string_view, kMaxFactions> kFactionKeys{
    "red", "blue", "green", "yellow", "purple", "orange", "cyan", "magenta"};

// The JSON key of a faction (never null).
[[nodiscard]] constexpr std::string_view faction_key(Faction faction) noexcept {
  return kFactionKeys[static_cast<std::size_t>(faction)];
}

// The faction for a scenario key, or nullopt for any other spelling.
[[nodiscard]] constexpr std::optional<Faction> faction_from_key(
    std::string_view key) noexcept {
  for (std::size_t i = 0; i < kFactionKeys.size(); ++i) {
    if (key == kFactionKeys[i]) {
      return static_cast<Faction>(i);
    }
  }
  return std::nullopt;
}

// Sector direction table (M14 items 2/4): [n][i] = (cos, sin) of
// angle = pi + 2*pi*i/n for every faction count n in [2, 8] and
// faction index i < n. Stored as hex float literals so the spawn
// anchors use identical bits on every platform and libm — a
// runtime cos/sin can differ in the last ulp between libms and
// would shift every multi-faction spawn position. Generated once;
// the literals are the source of truth (unused slots zero).
inline constexpr std::array<std::array<std::array<double, 2>, 8>, 9>
    kSectorDirections{{
    {},
    {},  // n < 2 unused
    {/* n = 2 */ {{-0x1.0000000000000p+0, 0x1.1a62633145c07p-53}, {0x1.0000000000000p+0, -0x1.1a62633145c07p-52}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}}},
    {/* n = 3 */ {{-0x1.0000000000000p+0, 0x1.1a62633145c07p-53}, {0x1.ffffffffffff4p-2, -0x1.bb67ae8584caep-1}, {0x1.0000000000005p-1, 0x1.bb67ae8584ca8p-1}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}}},
    {/* n = 4 */ {{-0x1.0000000000000p+0, 0x1.1a62633145c07p-53}, {-0x1.a79394c9e8a0ap-53, -0x1.0000000000000p+0}, {0x1.0000000000000p+0, -0x1.1a62633145c07p-52}, {0x1.60fafbfd97309p-52, 0x1.0000000000000p+0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}}},
    {/* n = 5 */ {{-0x1.0000000000000p+0, 0x1.1a62633145c07p-53}, {-0x1.3c6ef372fe952p-2, -0x1.e6f0e134454ffp-1}, {0x1.9e3779b97f4a7p-1, -0x1.2cf2304755a60p-1}, {0x1.9e3779b97f4a9p-1, 0x1.2cf2304755a5cp-1}, {-0x1.3c6ef372fe94ap-2, 0x1.e6f0e13445500p-1}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}}},
    {/* n = 6 */ {{-0x1.0000000000000p+0, 0x1.1a62633145c07p-53}, {-0x1.0000000000004p-1, -0x1.bb67ae8584ca8p-1}, {0x1.ffffffffffff4p-2, -0x1.bb67ae8584caep-1}, {0x1.0000000000000p+0, -0x1.1a62633145c07p-52}, {0x1.0000000000005p-1, 0x1.bb67ae8584ca8p-1}, {-0x1.0000000000007p-1, 0x1.bb67ae8584ca7p-1}, {0.0, 0.0}, {0.0, 0.0}}},
    {/* n = 7 */ {{-0x1.0000000000000p+0, 0x1.1a62633145c07p-53}, {-0x1.3f3a0e28bedd3p-1, -0x1.904c37505de4ap-1}, {0x1.c7b90e302457cp-3, -0x1.f329c0558e969p-1}, {0x1.cd4bca9cb5c70p-1, -0x1.bc4c04d71abc5p-2}, {0x1.cd4bca9cb5c72p-1, 0x1.bc4c04d71abbdp-2}, {0x1.c7b90e302458dp-3, 0x1.f329c0558e968p-1}, {-0x1.3f3a0e28bedcfp-1, 0x1.904c37505de4dp-1}, {0.0, 0.0}}},
    {/* n = 8 */ {{-0x1.0000000000000p+0, 0x1.1a62633145c07p-53}, {-0x1.6a09e667f3bcep-1, -0x1.6a09e667f3bccp-1}, {-0x1.a79394c9e8a0ap-53, -0x1.0000000000000p+0}, {0x1.6a09e667f3bcbp-1, -0x1.6a09e667f3bcep-1}, {0x1.0000000000000p+0, -0x1.1a62633145c07p-52}, {0x1.6a09e667f3bcep-1, 0x1.6a09e667f3bcbp-1}, {0x1.60fafbfd97309p-52, 0x1.0000000000000p+0}, {-0x1.6a09e667f3bc5p-1, 0x1.6a09e667f3bd4p-1}}},
}};

[[nodiscard]] const char* to_string(Faction faction) noexcept;

}  // namespace sim
