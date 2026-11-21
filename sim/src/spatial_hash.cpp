#include "sim/spatial_hash.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "sim/factions.hpp"

namespace sim {
namespace {

[[noreturn]] void fail(std::string message) {
  throw std::invalid_argument(std::move(message));
}

// Sentinel stored in unit_cell_ for units the grid does not track.
constexpr std::int32_t kUntracked = -1;

// Maps a pre-divided coordinate to a cell index in [0, extent - 1].
// Negative, NaN and overlarge values clamp to the border cells (see the
// exactness note in the header: clamping keeps the ring-pruning bound valid).
std::size_t clamp_to_index(double value, std::size_t extent) noexcept {
  if (extent == 0) {
    return 0;
  }
  const double index = std::floor(value);
  if (!(index > 0.0)) {
    return 0;  // negative, zero or NaN
  }
  const double capped = std::min(index, static_cast<double>(extent - 1));
  return static_cast<std::size_t>(capped);
}

}  // namespace

void SpatialHashGrid::configure(double width, double height, double cell_size,
                                std::size_t reserve_per_cell,
                                std::size_t faction_count) {
  if (!(cell_size > 0.0) || !std::isfinite(cell_size)) {
    fail("grid cell size must be a finite number > 0");
  }
  if (!(width > 0.0) || !(height > 0.0) || !std::isfinite(width) ||
      !std::isfinite(height)) {
    fail("grid extents must be finite numbers > 0");
  }
  if (faction_count < kFactionCount || faction_count > kMaxFactions) {
    fail("grid faction count must be in [2, " +
         std::to_string(kMaxFactions) + "] (got " +
         std::to_string(faction_count) + ")");
  }
  const double cols_d = std::ceil(width / cell_size);
  const double rows_d = std::ceil(height / cell_size);
  if (!std::isfinite(cols_d) || !std::isfinite(rows_d) || cols_d < 1.0 ||
      rows_d < 1.0 || cols_d * rows_d > static_cast<double>(kMaxGridCells)) {
    fail("grid cell size " + std::to_string(cell_size) +
         " needs too many cells for a " + std::to_string(width) + " x " +
         std::to_string(height) + " battlefield (limit " +
         std::to_string(kMaxGridCells) + " cells)");
  }

  width_ = width;
  height_ = height;
  cell_size_ = cell_size;
  cols_ = static_cast<std::size_t>(cols_d);
  rows_ = static_cast<std::size_t>(rows_d);
  faction_count_ = faction_count;
  // Flat buckets: cell * faction_count_ + faction (see the header). A
  // 2-faction grid lays out exactly the [faction 0, faction 1] buckets the
  // per-cell std::array had, in the same order.
  buckets_.assign(cols_ * rows_ * faction_count_, {});
  for (std::vector<std::uint32_t>& bucket : buckets_) {
    bucket.reserve(reserve_per_cell);
  }
  // Occupancy bitmaps: row-major (one row of cols_ bits per row) and
  // column-major (one column of rows_ bits per column), per faction.
  row_words_ = (cols_ + 63) / 64;
  col_words_ = (rows_ + 63) / 64;
  for (std::size_t f = 0; f < faction_count_; ++f) {
    occupancy_[f][0].assign(row_words_ * rows_, 0);
    occupancy_[f][1].assign(col_words_ * cols_, 0);
  }
  unit_cell_.clear();
  unit_slot_.clear();
  shadow_.clear();
  tracked_count_ = 0;
}

// Cell bucket index for a position: cell (col, row) -> col * rows + row.
std::size_t SpatialHashGrid::cell_of(double x, double y) const noexcept {
  return clamp_to_index(x / cell_size_, cols_) * rows_ +
         clamp_to_index(y / cell_size_, rows_);
}

void SpatialHashGrid::set_cell_occupancy(std::size_t cell, Faction faction,
                                         bool set) {
  const std::size_t x = cell / rows_;
  const std::size_t y = cell % rows_;
  const std::size_t f = static_cast<std::size_t>(faction);
  const std::uint64_t bit = 1ULL << (x % 64);
  if (set) {
    occupancy_[f][0][y * row_words_ + x / 64] |= bit;
  } else {
    occupancy_[f][0][y * row_words_ + x / 64] &= ~bit;
  }
  const std::uint64_t col_bit = 1ULL << (y % 64);
  if (set) {
    occupancy_[f][1][x * col_words_ + y / 64] |= col_bit;
  } else {
    occupancy_[f][1][x * col_words_ + y / 64] &= ~col_bit;
  }
}

bool SpatialHashGrid::cell_has_units(Faction faction, std::int64_t x,
                                     std::int64_t y) const noexcept {
  if (x < 0 || y < 0 || x >= static_cast<std::int64_t>(cols_) ||
      y >= static_cast<std::int64_t>(rows_)) {
    return false;
  }
  const std::size_t f = static_cast<std::size_t>(faction);
  const std::size_t ux = static_cast<std::size_t>(x);
  const std::size_t uy = static_cast<std::size_t>(y);
  return (occupancy_[f][0][uy * row_words_ + ux / 64] >> (ux % 64)) & 1ULL;
}

// True when any cell along a row (column = false: `fixed` is the row, the
// range runs over columns) or column (column = true: `fixed` is the column,
// the range runs over rows) in the inclusive range holds units of the
// faction. Whole 64-cell words are tested at once, so long empty stretches
// cost a couple of loads.
bool SpatialHashGrid::segment_has_units(Faction faction, bool column,
                                        std::int64_t along_from,
                                        std::int64_t along_to,
                                        std::int64_t fixed) const noexcept {
  const std::int64_t fixed_extent =
      static_cast<std::int64_t>(column ? cols_ : rows_);
  const std::int64_t along_extent =
      static_cast<std::int64_t>(column ? rows_ : cols_);
  if (fixed < 0 || fixed >= fixed_extent) {
    return false;
  }
  along_from = std::max<std::int64_t>(along_from, 0);
  along_to = std::min<std::int64_t>(along_to, along_extent - 1);
  if (along_from > along_to) {
    return false;
  }
  const std::size_t words = column ? col_words_ : row_words_;
  const std::size_t f = static_cast<std::size_t>(faction);
  const std::uint64_t* bits = occupancy_[f][column ? 1 : 0].data() +
                              static_cast<std::size_t>(fixed) * words;
  for (std::int64_t word = along_from / 64; word <= along_to / 64; ++word) {
    std::uint64_t value = bits[word];
    const std::int64_t word_start = word * 64;
    if (along_from > word_start) {
      value &= ~((1ULL << (along_from - word_start)) - 1ULL);
    }
    const std::int64_t word_last = word_start + 63;
    if (along_to < word_last) {
      value &= (1ULL << (along_to - word_start + 1)) - 1ULL;
    }
    if (value != 0) {
      return true;
    }
  }
  return false;
}

void SpatialHashGrid::refresh_shadow(const std::vector<Unit>& units,
                                     std::uint32_t index) {
  if (shadow_.size() < units.size()) {
    shadow_.resize(units.size());
  }
  const Unit& unit = units[index];
  Shadow& s = shadow_[index];
  s.x = unit.x;
  s.y = unit.y;
  s.id = unit.id;
  s.faction = static_cast<std::uint8_t>(unit.faction);
  s.alive = unit.alive;
}

void SpatialHashGrid::insert_into_cell(std::uint32_t index, std::size_t cell,
                                       Faction faction) {
  if (buckets_.empty()) {
    fail("spatial hash grid is not configured");
  }
  std::vector<std::uint32_t>& bucket =
      bucket_of(cell, static_cast<std::size_t>(faction));
  if (bucket.empty()) {
    set_cell_occupancy(cell, faction, true);
  }
  bucket.push_back(index);
  if (index >= unit_cell_.size()) {
    unit_cell_.resize(static_cast<std::size_t>(index) + 1, kUntracked);
    unit_slot_.resize(static_cast<std::size_t>(index) + 1, 0u);
  }
  unit_cell_[index] = static_cast<std::int32_t>(cell);
  unit_slot_[index] = static_cast<std::uint32_t>(bucket.size() - 1);
  ++tracked_count_;
}

void SpatialHashGrid::build(const std::vector<Unit>& units) {
  if (buckets_.empty()) {
    fail("spatial hash grid is not configured");
  }
  for (std::vector<std::uint32_t>& bucket : buckets_) {
    bucket.clear();  // keeps capacity
  }
  for (std::size_t f = 0; f < faction_count_; ++f) {
    occupancy_[f][0].assign(row_words_ * rows_, 0);
    occupancy_[f][1].assign(col_words_ * cols_, 0);
  }
  unit_cell_.assign(units.size(), kUntracked);
  unit_slot_.assign(units.size(), 0u);
  // The shadow covers every unit, alive or not: dead records are never
  // returned by a query (dead units are not tracked), but keeping the
  // array in lockstep with `units` makes shadow_of() valid for any index
  // the caller can name.
  shadow_.resize(units.size());
  tracked_count_ = 0;
  for (std::size_t i = 0; i < units.size(); ++i) {
    refresh_shadow(units, static_cast<std::uint32_t>(i));
    if (units[i].alive) {
      insert_into_cell(static_cast<std::uint32_t>(i),
                       cell_of(units[i].x, units[i].y), units[i].faction);
    }
  }
}

void SpatialHashGrid::insert(const std::vector<Unit>& units,
                             std::uint32_t index) {
  if (index >= units.size()) {
    return;
  }
  if (index < unit_cell_.size() && unit_cell_[index] != kUntracked) {
    return;  // already tracked
  }
  const Unit& unit = units[index];
  refresh_shadow(units, index);
  insert_into_cell(index, cell_of(unit.x, unit.y), unit.faction);
}

void SpatialHashGrid::remove(const std::vector<Unit>& units,
                             std::uint32_t index) {
  if (index >= units.size() || index >= unit_cell_.size() ||
      index >= unit_slot_.size()) {
    return;
  }
  const std::int32_t cell = unit_cell_[index];
  if (cell == kUntracked) {
    return;
  }
  std::vector<std::uint32_t>& bucket =
      bucket_of(static_cast<std::size_t>(cell),
                static_cast<std::size_t>(units[index].faction));
  const std::uint32_t slot = unit_slot_[index];
  if (slot < bucket.size() && bucket[slot] == index) {
    // Swap-remove through the tracked slot: the identical writes the
    // std::find-based removal performed (bucket order changes, but query
    // results do not depend on it — see the class comment). The displaced
    // tail element's slot is corrected; when the removed element IS the
    // tail, the write is the same self-assignment the old code did.
    const std::uint32_t moved = bucket.back();
    bucket[slot] = moved;
    if (slot + 1 < bucket.size()) {
      unit_slot_[moved] = slot;
    }
    bucket.pop_back();
    if (bucket.empty()) {
      set_cell_occupancy(static_cast<std::size_t>(cell), units[index].faction,
                         false);
    }
  }
  unit_cell_[index] = kUntracked;
  --tracked_count_;
  refresh_shadow(units, index);
}

void SpatialHashGrid::update(const std::vector<Unit>& units,
                             std::uint32_t index) {
  if (index >= units.size() || !units[index].alive) {
    return;  // dead units are never tracked
  }
  // The shadow follows the authoritative position even when the cell did
  // not change — a within-cell move still moves the unit, and the next
  // query in the same tick must see the new position.
  refresh_shadow(units, index);
  if (index >= unit_cell_.size() || unit_cell_[index] == kUntracked) {
    const Unit& unit = units[index];
    insert_into_cell(index, cell_of(unit.x, unit.y), unit.faction);
    return;
  }
  const std::size_t cell = cell_of(units[index].x, units[index].y);
  if (static_cast<std::size_t>(unit_cell_[index]) == cell) {
    return;  // same cell: nothing to do
  }
  remove(units, index);
  insert_into_cell(index, cell, units[index].faction);
}

bool SpatialHashGrid::scan_cell(const std::vector<Unit>& units,
                                std::size_t cell, Faction own, double qx,
                                double qy, int& best, double& best_d2) const {
  bool improved = false;
  // Every faction except `own` is hostile (M14 item 2). Factions are
  // scanned in index order; the minimum over (squared distance, unit id)
  // is unique, so the scan order can never change the answer. A 2-faction
  // battle scans exactly the one enemy bucket the old code did.
  for (std::size_t f = 0; f < faction_count_; ++f) {
    if (f == static_cast<std::size_t>(own)) {
      continue;
    }
    for (std::uint32_t candidate : bucket_of(cell, f)) {
      const Unit& other = units[candidate];
      if (!other.alive) {
        continue;  // defense in depth: see the class comment
      }
      const double dx = other.x - qx;
      const double dy = other.y - qy;
      const double d2 = dx * dx + dy * dy;
      // Exactly the brute-force rule: strictly closer wins; exact ties go to
      // the lower unit id, independent of scan order.
      if (best < 0 || d2 < best_d2 ||
          (d2 == best_d2 &&
           other.id < units[static_cast<std::size_t>(best)].id)) {
        best = static_cast<int>(candidate);
        best_d2 = d2;
        improved = true;
      }
    }
  }
  return improved;
}

int SpatialHashGrid::nearest_enemy(const std::vector<Unit>& units,
                                   const Unit& unit) const {
  if (buckets_.empty() || tracked_count_ == 0) {
    return -1;
  }
  const std::size_t own = static_cast<std::size_t>(unit.faction);
  // Ring pruning asks "any hostile faction here?" — for a 2-faction battle
  // that is exactly the old single-enemy bitmap test, in the same order.
  const auto any_segment_units = [&](bool column, std::int64_t from,
                                     std::int64_t to, std::int64_t fixed) {
    for (std::size_t f = 0; f < faction_count_; ++f) {
      if (f == own) {
        continue;
      }
      if (segment_has_units(static_cast<Faction>(f), column, from, to,
                            fixed)) {
        return true;
      }
    }
    return false;
  };
  const auto any_cell_units = [&](std::int64_t x, std::int64_t y) {
    for (std::size_t f = 0; f < faction_count_; ++f) {
      if (f == own) {
        continue;
      }
      if (cell_has_units(static_cast<Faction>(f), x, y)) {
        return true;
      }
    }
    return false;
  };
  const std::size_t cx = clamp_to_index(unit.x / cell_size_, cols_);
  const std::size_t cy = clamp_to_index(unit.y / cell_size_, rows_);

  int best = -1;
  double best_d2 = 0.0;

  // Ring 0: the unit's own cell.
  scan_cell(units, cx * rows_ + cy, unit.faction, unit.x, unit.y, best,
            best_d2);

  // Rings 1, 2, ... — each ring is the border of the (2r+1)^2 block around
  // the query cell: the top and bottom rows (corners included) plus the left
  // and right columns (corners excluded), all clamped to the grid. A ring is
  // skipped only when every point in it is strictly farther than the best
  // candidate so far, so an equal-distance, lower-id candidate in a skipped
  // ring cannot exist.
  const std::size_t max_ring =
      std::max({cx, cy, cols_ - 1 - cx, rows_ - 1 - cy});
  for (std::size_t r = 1; r <= max_ring; ++r) {
    const double min_gap = static_cast<double>(r - 1) * cell_size_;
    if (best >= 0 && min_gap * min_gap > best_d2) {
      break;  // every remaining candidate is strictly worse
    }
    const std::int64_t x_lo = static_cast<std::int64_t>(cx) - static_cast<std::int64_t>(r);
    const std::int64_t x_hi = static_cast<std::int64_t>(cx) + static_cast<std::int64_t>(r);
    const std::int64_t y_lo = static_cast<std::int64_t>(cy) - static_cast<std::int64_t>(r);
    const std::int64_t y_hi = static_cast<std::int64_t>(cy) + static_cast<std::int64_t>(r);
    // Top and bottom rows, corners included. The occupancy bitmap skips
    // whole empty row segments with a few word loads.
    for (std::int64_t y : {y_lo, y_hi}) {
      if (!any_segment_units(/*column=*/false, x_lo, x_hi, y)) {
        continue;
      }
      const std::int64_t x_from = std::max<std::int64_t>(x_lo, 0);
      const std::int64_t x_to =
          std::min<std::int64_t>(x_hi, static_cast<std::int64_t>(cols_) - 1);
      for (std::int64_t x = x_from; x <= x_to; ++x) {
        if (any_cell_units(x, y)) {
          scan_cell(units, static_cast<std::size_t>(x) * rows_ +
                               static_cast<std::size_t>(y),
                    unit.faction, unit.x, unit.y, best, best_d2);
        }
      }
    }
    // Left and right columns, corners excluded (column-major words).
    for (std::int64_t x : {x_lo, x_hi}) {
      if (!any_segment_units(/*column=*/true, y_lo + 1, y_hi - 1, x)) {
        continue;
      }
      const std::int64_t y_from = std::max<std::int64_t>(y_lo + 1, 0);
      const std::int64_t y_to =
          std::min<std::int64_t>(y_hi - 1, static_cast<std::int64_t>(rows_) - 1);
      for (std::int64_t y = y_from; y <= y_to; ++y) {
        if (any_cell_units(x, y)) {
          scan_cell(units, static_cast<std::size_t>(x) * rows_ +
                               static_cast<std::size_t>(y),
                    unit.faction, unit.x, unit.y, best, best_d2);
        }
      }
    }
  }
  return best;
}

void SpatialHashGrid::append_within_radius(
    const std::vector<std::uint32_t>& bucket, double x, double y,
    double radius_sq, std::vector<std::uint32_t>& out) const {
  for (std::uint32_t candidate : bucket) {
    const Shadow& s = shadow_[candidate];
    if (!s.alive) {
      continue;  // shadow liveness: see the class comment
    }
    const double dx = s.x - x;
    const double dy = s.y - y;
    if (dx * dx + dy * dy <= radius_sq) {
      out.push_back(candidate);
    }
  }
}

void SpatialHashGrid::radius_query(double x, double y, double radius,
                                   std::vector<std::uint32_t>& out) const {
  out.clear();
  if (buckets_.empty() || !(radius >= 0.0)) {
    return;
  }
  // Cells overlapping the circle's bounding box (clamped); every candidate
  // is then checked against the exact inclusive radius, reading the
  // per-unit shadow instead of the unit array (see the class comment).
  const std::int64_t cx =
      static_cast<std::int64_t>(clamp_to_index(x / cell_size_, cols_));
  const std::int64_t cy =
      static_cast<std::int64_t>(clamp_to_index(y / cell_size_, rows_));
  const std::int64_t span =
      static_cast<std::int64_t>(std::ceil(radius / cell_size_));
  const std::int64_t x_from = std::max<std::int64_t>(cx - span, 0);
  const std::int64_t x_to =
      std::min<std::int64_t>(cx + span, static_cast<std::int64_t>(cols_) - 1);
  const std::int64_t y_from = std::max<std::int64_t>(cy - span, 0);
  const std::int64_t y_to =
      std::min<std::int64_t>(cy + span, static_cast<std::int64_t>(rows_) - 1);
  const double radius_sq = radius * radius;
  // M15 S5: skip (cell, faction) buckets the occupancy bitmap reports
  // empty — an empty bucket appends nothing, and the bitmap bit is set
  // exactly while the bucket is non-empty (insert_into_cell/remove/build
  // maintain it under the same invariants this query relies on), so the
  // appended sequence is unchanged while sparse regions stop paying the
  // bucket-header cache miss per cell per faction.
  for (std::int64_t cell_x = x_from; cell_x <= x_to; ++cell_x) {
    for (std::int64_t cell_y = y_from; cell_y <= y_to; ++cell_y) {
      for (std::size_t f = 0; f < faction_count_; ++f) {
        if (!cell_has_units(static_cast<Faction>(f), cell_x, cell_y)) {
          continue;
        }
        append_within_radius(
            bucket_of(static_cast<std::size_t>(cell_x) * rows_ +
                          static_cast<std::size_t>(cell_y),
                      f),
            x, y, radius_sq, out);
      }
    }
  }
}

void SpatialHashGrid::radius_query_faction(double x, double y, double radius,
                                           Faction faction,
                                           std::vector<std::uint32_t>& out) const {
  out.clear();
  if (buckets_.empty() || !(radius >= 0.0)) {
    return;
  }
  // Identical to radius_query but only the one faction's bucket per cell:
  // the result is the same subsequence the all-faction scan would have
  // produced for that faction, so a caller that filters by faction gets the
  // identical sequence without touching the other buckets.
  const std::int64_t cx =
      static_cast<std::int64_t>(clamp_to_index(x / cell_size_, cols_));
  const std::int64_t cy =
      static_cast<std::int64_t>(clamp_to_index(y / cell_size_, rows_));
  const std::int64_t span =
      static_cast<std::int64_t>(std::ceil(radius / cell_size_));
  const std::int64_t x_from = std::max<std::int64_t>(cx - span, 0);
  const std::int64_t x_to =
      std::min<std::int64_t>(cx + span, static_cast<std::int64_t>(cols_) - 1);
  const std::int64_t y_from = std::max<std::int64_t>(cy - span, 0);
  const std::int64_t y_to =
      std::min<std::int64_t>(cy + span, static_cast<std::int64_t>(rows_) - 1);
  const double radius_sq = radius * radius;
  const std::size_t f = static_cast<std::size_t>(faction);
  for (std::int64_t cell_x = x_from; cell_x <= x_to; ++cell_x) {
    for (std::int64_t cell_y = y_from; cell_y <= y_to; ++cell_y) {
      // M15 S5: the same occupancy skip as radius_query (the faction's
      // bucket only) — identical appended sequence, fewer bucket headers.
      if (!cell_has_units(faction, cell_x, cell_y)) {
        continue;
      }
      append_within_radius(
          bucket_of(static_cast<std::size_t>(cell_x) * rows_ +
                        static_cast<std::size_t>(cell_y),
                    f),
          x, y, radius_sq, out);
    }
  }
}

// M15 S1: the invariant checker's grid half. Read-only; see the header.
bool SpatialHashGrid::verify_contents(
    const std::vector<Unit>& units,
    std::vector<std::string>& violations) const {
  const std::size_t max_messages = 8;
  const auto push = [&](std::string message) {
    if (violations.size() < max_messages) {
      violations.push_back(std::move(message));
    }
  };
  bool ok = true;
  std::size_t alive_units = 0;
  for (std::uint32_t i = 0; i < units.size(); ++i) {
    const Unit& unit = units[i];
    if (unit.alive) {
      ++alive_units;
    }
    if (i >= unit_cell_.size()) {
      break;  // beyond anything the grid has ever tracked
    }
    const std::int32_t cell = unit_cell_[i];
    // Built only when a message is pushed: a healthy check must not
    // allocate (the allocation-counting tests measure steady-state
    // ticks with the checker on).
    const auto prefix = [&unit, i] {
      return "unit index " + std::to_string(i) + " (id " +
             std::to_string(unit.id) + "): ";
    };
    if (unit.alive) {
      if (cell < 0) {
        push(prefix() + "alive but not tracked");
        ok = false;
        continue;
      }
      const std::size_t expected = cell_of(unit.x, unit.y);
      if (static_cast<std::size_t>(cell) != expected) {
        push(prefix() + "tracked in cell " + std::to_string(cell) +
             " but its position is in cell " + std::to_string(expected));
        ok = false;
        continue;
      }
      const std::size_t faction = static_cast<std::size_t>(unit.faction);
      if (faction >= faction_count_) {
        // Reported by the simulation-level faction-valid invariant; the
        // bucket lookup below would index out of range, so stop here.
        continue;
      }
      const std::vector<std::uint32_t>& bucket =
          bucket_of(static_cast<std::size_t>(cell), faction);
      const std::uint32_t slot =
          i < unit_slot_.size() ? unit_slot_[i] : ~0u;
      if (slot >= bucket.size() || bucket[slot] != i) {
        push(prefix() + "bucket slot back-pointer does not lead to it");
        ok = false;
      }
      if (i < shadow_.size()) {
        // Queries read the shadows of bucketed (alive) units; a dead
        // unit's shadow is never consulted (and is default-constructed
        // for units that were dead before the grid ever saw them), so
        // only the alive records are verified.
        const Shadow& shadow = shadow_[i];
        if (shadow.x != unit.x || shadow.y != unit.y ||
            shadow.id != unit.id ||
            shadow.faction != static_cast<std::uint8_t>(unit.faction) ||
            !shadow.alive) {
          push(prefix() + "query shadow is stale");
          ok = false;
        }
      }
    } else if (cell >= 0) {
      push(prefix() + "dead but still tracked in cell " +
           std::to_string(cell));
      ok = false;
    }
  }
  std::size_t bucket_total = 0;
  for (std::size_t index = 0; index < buckets_.size(); ++index) {
    const std::vector<std::uint32_t>& bucket = buckets_[index];
    bucket_total += bucket.size();
    // M15 S5: the occupancy skip in radius_query/_faction trusts the
    // bitmap exactly — bit set iff the bucket holds units. A clear bit
    // over a non-empty bucket would silently drop candidates from every
    // query touching that cell; a set bit over an empty bucket is only
    // wasted work. Check both directions so the checker-on test builds
    // guard the skip's invariant every check interval. The message is
    // built only when reporting (healthy checks must not allocate).
    const bool has_units = cell_has_units(
        static_cast<Faction>(index % faction_count_),
        static_cast<std::int64_t>(index / faction_count_ / rows_),
        static_cast<std::int64_t>(index / faction_count_ % rows_));
    if (bucket.empty() == has_units) {
      const std::size_t cell = index / faction_count_;
      push("cell (col " + std::to_string(cell / rows_) + ", row " +
           std::to_string(cell % rows_) + ") faction " +
           std::to_string(index % faction_count_) +
           ": occupancy bitmap disagrees with bucket emptiness");
      ok = false;
    }
  }
  if (bucket_total != tracked_count_) {
    push("bucket entries total " + std::to_string(bucket_total) +
         " != tracked_count " + std::to_string(tracked_count_));
    ok = false;
  }
  if (tracked_count_ != alive_units) {
    push("tracked_count " + std::to_string(tracked_count_) +
         " != alive units " + std::to_string(alive_units));
    ok = false;
  }
  return ok;
}

}  // namespace sim
