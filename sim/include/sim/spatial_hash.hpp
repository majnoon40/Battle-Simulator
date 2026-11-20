#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "sim/factions.hpp"
#include "sim/unit.hpp"

namespace sim {

// Sanity limit on the grid size (cells), so a tiny cell size on a large
// battlefield fails validation instead of exhausting memory.
inline constexpr std::size_t kMaxGridCells = 1'048'576;

// Uniform spatial hash grid over a bounded battlefield (M3).
//
// The battlefield is partitioned into cols x rows square cells of side
// `cell_size`; cell (col, row) maps to the bucket index col * rows + row —
// a perfect hash, so there are no hash collisions and no unordered_map (the
// grid is fully deterministic by construction). Each cell keeps one bucket
// of tracked unit indices per faction.
//
// The grid stores INDICES into the caller's unit vector, which remains the
// authoritative state (positions, factions, ids, alive flags are read from
// it). The grid itself only tracks which units are in which cell; dead units
// are never inserted (the simulation removes them on death).
//
// Determinism: cells are addressed and iterated in fixed (row, col) order,
// and the simulation inserts/updates/removes in unit-index order. Query
// results are additionally independent of the intra-bucket order: the
// nearest-enemy minimum over (squared distance, unit id) is unique because
// ids are unique, so bucket layout can never change an answer.
//
// Liveness: the simulation removes dead units from the grid on death.
// nearest_enemy additionally re-checks the `alive` flag of every candidate
// against the authoritative unit array (it sits on the same cache line as
// the position) — so even state mutated behind the simulation's back yields
// exactly the brute-force answer. radius_query instead reads the
// grid-maintained shadow record (see the query-shadow note below), whose
// liveness is exactly as current as the buckets themselves — which, in the
// simulation, is always: every death flows through remove() in the same
// apply_deaths() iteration that clears the flag.
//
// Exactness: nearest_enemy returns the same unit as the brute-force
// sim::nearest_enemy_index — nearest squared distance, ties broken by the
// lower unit id. Expanding Chebyshev rings of cells are scanned outward from
// the query's cell; a ring is skipped only when every point in it is
// STRICTLY farther than the best candidate found so far, so an equal-distance
// lower-id unit in that ring is still found. Ring pruning bound: every point
// in a ring-r cell is at squared distance >= ((r - 1) * cell_size)^2 from
// any point in the query's cell (ring 1: cells touch, bound 0). Coordinates
// outside [0, width] x [0, height] are clamped into the border cells; a
// clamped unit's true position is never closer to any other cell than its
// clamped cell, so the pruning bound stays valid for out-of-world units and
// queries, and the result is exact either way.
//
// Steady-state allocations: buckets are reserved at configure() time
// (`reserve_per_cell`) and reuse their capacity afterwards; swap-remove and
// push_back never shrink, so a warmed-up grid stops allocating. This is
// verified by an allocation-counting test and the benchmark harness.
//
// Query shadow (m14 item 1): every radius_query reads its candidates from a
// per-unit shadow record maintained by the grid itself (build/insert/update/
// remove copy the fields queries test — position, id, faction, alive — from
// the caller's unit array on every operation for that unit). The
// simulation funnels every position change through update() (once per moved
// unit, after the movement commit) and every death through remove(), so the
// shadow is current whenever a query runs, and a query never touches the
// 232-byte Unit struct: it reads this 24-byte record instead. The values
// are copies and the bucket iteration order is unchanged, so query outputs
// are bit-identical to reading the unit array directly. The liveness
// contract above (re-checking the authoritative array) is kept by
// nearest_enemy, which still reads the unit array; radius_query's liveness
// is the shadow, which is exactly as current as the buckets are.
class SpatialHashGrid {
 public:
  // The per-unit query shadow (see the class comment). `faction` is the
  // numeric faction value; compare against static_cast<std::uint8_t>(...).
  struct Shadow {
    double x = 0.0;
    double y = 0.0;
    std::uint32_t id = 0;
    std::uint8_t faction = 0;
    bool alive = false;
  };

  SpatialHashGrid() = default;

  // (Re-)configures the grid for a battlefield. `faction_count` (M14 item 2)
  // sizes the per-cell buckets and occupancy bitmaps; it must be in
  // [2, kMaxFactions] and defaults to 2 (the legacy game — identical bucket
  // layout and query order to before). Throws std::invalid_argument for a
  // non-positive or non-finite cell size, non-positive extents, a cell count
  // above kMaxGridCells, or a faction count outside [2, kMaxFactions].
  // Clears all tracked units.
  void configure(double width, double height, double cell_size,
                 std::size_t reserve_per_cell,
                 std::size_t faction_count = kFactionCount);

  // Clears every bucket (keeping capacity) and inserts every alive unit of
  // `units` in index order. The canonical fixed-order rebuild.
  void build(const std::vector<Unit>& units);

  // Starts tracking an alive unit (index into `units`).
  void insert(const std::vector<Unit>& units, std::uint32_t index);

  // Stops tracking a unit (swap-removes it from its bucket). No-op when the
  // unit is not tracked.
  void remove(const std::vector<Unit>& units, std::uint32_t index);

  // Moves a tracked unit to the bucket matching its current position, if the
  // cell changed. Inserts the unit when it is not tracked yet.
  void update(const std::vector<Unit>& units, std::uint32_t index);

  // Index of the nearest tracked enemy of `unit` (exact brute-force
  // semantics: minimum squared distance, ties to the lower unit id), or -1
  // when the enemy faction has no tracked units.
  [[nodiscard]] int nearest_enemy(const std::vector<Unit>& units,
                                  const Unit& unit) const;

  // All tracked units within `radius` (inclusive) of (x, y), in fixed cell
  // order (row-major, then bucket order). Clears `out` first; the caller
  // reuses the buffer, so repeated queries do not allocate. Reads candidates
  // from the grid-maintained shadow (see the class comment).
  void radius_query(double x, double y, double radius,
                    std::vector<std::uint32_t>& out) const;

  // Same, restricted to one faction's buckets. The result is exactly the
  // subsequence of radius_query's output that carries `faction` — callers
  // that filter by faction afterwards get the identical sequence, and the
  // other faction's buckets are never touched.
  void radius_query_faction(double x, double y, double radius, Faction faction,
                            std::vector<std::uint32_t>& out) const;

  // The shadow record for a unit index (see the class comment). Valid for
  // any index below the unit count at the last build/insert; records for
  // never-tracked indices carry the values copied when they were first seen
  // (alive=false before that).
  [[nodiscard]] const Shadow& shadow_of(std::uint32_t index) const {
    return shadow_[index];
  }

  [[nodiscard]] double cell_size() const noexcept { return cell_size_; }
  [[nodiscard]] std::size_t cols() const noexcept { return cols_; }
  [[nodiscard]] std::size_t rows() const noexcept { return rows_; }
  [[nodiscard]] std::size_t cell_count() const noexcept {
    return cols_ * rows_;
  }
  // Number of units currently tracked (all factions).
  [[nodiscard]] std::size_t tracked_count() const noexcept {
    return tracked_count_;
  }
  // Cell bucket index for a position, clamped to the grid. Public for tests.
  [[nodiscard]] std::size_t cell_of(double x, double y) const noexcept;

 private:
  // Copies the query-relevant fields of units[index] into the shadow.
  void refresh_shadow(const std::vector<Unit>& units, std::uint32_t index);
  // Appends every alive bucket member within radius_sq of (x, y) to `out`,
  // reading the per-unit shadow (shared by both radius queries).
  void append_within_radius(const std::vector<std::uint32_t>& bucket,
                            double x, double y, double radius_sq,
                            std::vector<std::uint32_t>& out) const;
  void insert_into_cell(std::uint32_t index, std::size_t cell,
                        Faction faction);
  // Flat bucket storage: cell (col, row) x faction -> one bucket of unit
  // indices, bucket index = cell * faction_count_ + faction. Sized at
  // configure() to cell_count * faction_count_ — a 2-faction battle gets
  // exactly the layout the per-cell std::array had, in the same iteration
  // order (faction index 0 then 1), so outputs are bit-identical.
  [[nodiscard]] std::vector<std::uint32_t>& bucket_of(
      std::size_t cell, std::size_t faction) noexcept {
    return buckets_[cell * faction_count_ + faction];
  }
  [[nodiscard]] const std::vector<std::uint32_t>& bucket_of(
      std::size_t cell, std::size_t faction) const noexcept {
    return buckets_[cell * faction_count_ + faction];
  }
  // Occupancy bitmaps, one per faction and orientation: bit (x, y) is set
  // when cell (x, y) holds at least one unit of that faction. Ring scans
  // test whole row/column segments with a few word loads and only visit
  // cells whose bit is set, which keeps far-target queries (large rings of
  // empty cells) cheap. Both orientations coalesce: horizontal segments use
  // the row-major words, vertical segments the column-major words.
  [[nodiscard]] bool segment_has_units(Faction faction, bool column,
                                       std::int64_t along_from,
                                       std::int64_t along_to,
                                       std::int64_t fixed) const noexcept;
  [[nodiscard]] bool cell_has_units(Faction faction, std::int64_t x,
                                    std::int64_t y) const noexcept;
  void set_cell_occupancy(std::size_t cell, Faction faction, bool set);
  // Scans one cell's enemy buckets (every faction except `own`) against the
  // current best candidate. Returns true when the best candidate changed.
  bool scan_cell(const std::vector<Unit>& units, std::size_t cell,
                 Faction own, double qx, double qy, int& best,
                 double& best_d2) const;

  std::vector<std::vector<std::uint32_t>> buckets_;
  std::vector<std::int32_t> unit_cell_;  // unit index -> cell id, -1 = untracked
  // unit index -> slot inside its bucket (the position unit_cell_ points
  // at), maintained on every insert/swap-remove so remove() is O(1) instead
  // of scanning the bucket. Pure bookkeeping: the swap-remove performs the
  // exact same writes as the old std::find-based one, so bucket layouts are
  // identical.
  std::vector<std::uint32_t> unit_slot_;
  // The per-unit query shadow (see the class comment), parallel to the
  // caller's unit vector.
  std::vector<Shadow> shadow_;
  // Occupancy bitmaps per faction and orientation: [faction][0] row-major,
  // [faction][1] column-major. Bit (x, y) is set when cell (x, y) holds at
  // least one unit of that faction. Only [0, faction_count_) are used.
  std::vector<std::uint64_t> occupancy_[kMaxFactions][2];
  std::size_t faction_count_ = kFactionCount;
  std::size_t row_words_ = 0;  // words per row in the row-major bitmaps
  std::size_t col_words_ = 0;  // words per column in the column-major bitmaps
  double width_ = 0.0;
  double height_ = 0.0;
  double cell_size_ = 0.0;
  std::size_t cols_ = 0;
  std::size_t rows_ = 0;
  std::size_t tracked_count_ = 0;
};

}  // namespace sim
