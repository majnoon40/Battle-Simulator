// M3 spatial hash tests: property equivalence with the brute-force oracle
// over thousands of random states (ties, cell boundaries, world edges, dead
// units, empty factions, single-unit armies), incremental maintenance,
// radius queries, mode regressions (M1/M2 references unchanged in both
// neighbor modes), 5,000-unit determinism, and the steady-state
// allocation check.
#include "sim/config.hpp"
#include "sim/json_export.hpp"
#include "sim/neighbors.hpp"
#include "sim/simulation.hpp"
#include "sim/spatial_hash.hpp"
#include "sim/unit_types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "allocation_counter.hpp"

// The shared allocation counter (tests/allocation_counter.cpp) replaces
// operator new/delete for the whole test binary so the steady-state claim
// can be asserted directly. GoogleTest allocates between tests; the
// assertions only compare counts across a tick() window, outside which
// nothing runs.

namespace {

constexpr std::size_t kRed = static_cast<std::size_t>(sim::Faction::Red);
constexpr std::size_t kBlue = static_cast<std::size_t>(sim::Faction::Blue);

sim::Unit make_unit(std::uint32_t id, sim::Faction faction, double x, double y) {
  sim::Unit unit;
  unit.id = id;
  unit.faction = faction;
  unit.x = x;
  unit.y = y;
  unit.health = 100.0;
  unit.stats.max_health = 100.0;
  unit.stats.speed = 2.0;
  unit.stats.damage = 12.0;
  unit.stats.attack_range = 2.0;
  unit.stats.attack_cooldown_ticks = 5;
  unit.alive = true;
  return unit;
}

sim::UnitStats infantry_stats() {
  sim::UnitStats stats;
  stats.max_health = 100.0;
  stats.speed = 2.0;
  stats.damage = 12.0;
  stats.attack_range = 2.0;
  stats.attack_cooldown_ticks = 5;
  return stats;
}

// ---------------------------------------------------------------------------
// Property test: grid vs brute force on thousands of random states.
// ---------------------------------------------------------------------------

struct RandomState {
  std::vector<sim::Unit> units;
  double world = 0.0;
  double cell = 0.0;
};

RandomState random_state(std::uint64_t seed) {
  sim::Rng rng(seed);
  RandomState state;
  state.world = 20.0 + rng.next_double() * 380.0;
  const double cell_choices[] = {1.0, 2.5, 4.0, 7.0, 13.0, 32.0, 64.0};
  state.cell = cell_choices[rng.next_uint32(7)];
  const std::uint32_t count = 1 + rng.next_uint32(40);

  // Ids are shuffled against indices so the lowest-id tie-break is exercised
  // independently of the scan order.
  std::vector<std::uint32_t> ids(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    ids[i] = i + 1;
  }
  for (std::uint32_t i = count; i > 1; --i) {
    const std::uint32_t j = rng.next_uint32(i);
    std::swap(ids[i - 1], ids[j]);
  }

  state.units.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    // Position mix: uniform, exact cell-boundary multiples, exact world
    // edges, small overshoots beyond the edges (clamping must stay exact),
    // and exact duplicates of earlier positions (exact distance ties).
    double x = 0.0;
    double y = 0.0;
    const auto coord = [&]() -> double {
      const double roll = rng.next_double();
      if (roll < 0.55) {
        return rng.next_double() * state.world;
      }
      if (roll < 0.70) {
        const std::uint32_t k =
            rng.next_uint32(static_cast<std::uint32_t>(state.world /
                                                       state.cell) +
                            2);
        return std::min(static_cast<double>(k) * state.cell, state.world);
      }
      if (roll < 0.80) {
        return rng.next_double() < 0.5 ? 0.0 : state.world;
      }
      if (roll < 0.90) {
        return rng.next_double() * 2.0 * state.cell - state.cell;
      }
      return state.world + rng.next_double() * state.cell;
    };
    x = coord();
    y = coord();
    if (!state.units.empty() && rng.next_double() < 0.08) {
      const sim::Unit& other =
          state.units[rng.next_uint32(static_cast<std::uint32_t>(
              state.units.size()))];
      x = other.x;  // exact duplicate: guaranteed d2 ties
      y = other.y;
    }
    sim::Unit unit =
        make_unit(ids[i], rng.next_uint32(2) == 0 ? sim::Faction::Red
                                                   : sim::Faction::Blue,
                  x, y);
    unit.alive = rng.next_double() < 0.75;  // dead units must be ignored
    state.units.push_back(unit);
  }
  return state;
}

TEST(SpatialHashPropertyTest, NearestEnemyMatchesBruteForceOnRandomStates) {
  std::uint64_t comparisons = 0;
  for (std::uint64_t trial = 0; trial < 400; ++trial) {
    const RandomState state = random_state(0x5EED0000ULL + trial);
    sim::SpatialHashGrid grid;
    grid.configure(state.world, state.world, state.cell, 8);
    grid.build(state.units);
    for (std::size_t i = 0; i < state.units.size(); ++i) {
      const int brute =
          sim::nearest_enemy_index(state.units, state.units[i]);
      const int grid_result =
          grid.nearest_enemy(state.units, state.units[i]);
      ASSERT_EQ(grid_result, brute)
          << "trial " << trial << " (world " << state.world << ", cell "
          << state.cell << "), query unit index " << i << " id "
          << state.units[i].id << " at (" << state.units[i].x << ", "
          << state.units[i].y << ")";
      ++comparisons;
    }
  }
  // "Thousands of random states": 400 trials x ~20 units each.
  EXPECT_GE(comparisons, 5000u);
}

TEST(SpatialHashPropertyTest, RadiusQueryMatchesBruteForce) {
  for (std::uint64_t trial = 0; trial < 100; ++trial) {
    const RandomState state = random_state(0xC0FFEE00ULL + trial);
    sim::SpatialHashGrid grid;
    grid.configure(state.world, state.world, state.cell, 8);
    grid.build(state.units);

    const sim::Unit& anchor =
        state.units.empty() ? make_unit(0, sim::Faction::Red, 0.0, 0.0)
                            : state.units[0];
    const double cx = anchor.x + 0.5 * state.cell;
    const double cy = anchor.y;
    const double radius_choices[] = {0.0, 2.5, 10.0, 40.0, 200.0};
    const double radius = radius_choices[trial % 5];

    std::vector<std::uint32_t> found;
    grid.radius_query(cx, cy, radius, found);
    std::sort(found.begin(), found.end());

    std::vector<std::uint32_t> expected;
    const double radius_sq = radius * radius;
    for (std::size_t i = 0; i < state.units.size(); ++i) {
      const sim::Unit& unit = state.units[i];
      if (!unit.alive) {
        continue;  // only tracked (alive) units are ever returned
      }
      const double dx = unit.x - cx;
      const double dy = unit.y - cy;
      if (dx * dx + dy * dy <= radius_sq) {
        expected.push_back(static_cast<std::uint32_t>(i));
      }
    }
    std::sort(expected.begin(), expected.end());
    ASSERT_EQ(found, expected) << "trial " << trial << " radius " << radius;
  }
}

TEST(SpatialHashPropertyTest, IncrementalMaintenanceMatchesRebuildAndBrute) {
  sim::Rng rng(0xABCDEF01);
  const double world = 200.0;
  const double cell = 8.0;
  RandomState state;
  state.world = world;
  state.cell = cell;
  for (std::uint32_t i = 0; i < 30; ++i) {
    sim::Unit unit = make_unit(
        i + 1, rng.next_uint32(2) == 0 ? sim::Faction::Red : sim::Faction::Blue,
        rng.next_double() * world, rng.next_double() * world);
    state.units.push_back(unit);
  }

  sim::SpatialHashGrid grid;
  grid.configure(world, world, cell, 8);
  grid.build(state.units);

  for (int step = 0; step < 200; ++step) {
    // Pick a random alive unit and either move it or kill it.
    std::vector<std::uint32_t> alive;
    for (std::uint32_t i = 0; i < state.units.size(); ++i) {
      if (state.units[i].alive) {
        alive.push_back(i);
      }
    }
    if (alive.empty()) {
      break;
    }
    const std::uint32_t index = alive[rng.next_uint32(
        static_cast<std::uint32_t>(alive.size()))];
    if (step % 7 == 6) {
      state.units[index].alive = false;
      grid.remove(state.units, index);
    } else {
      sim::Unit& unit = state.units[index];
      unit.x = std::clamp(unit.x + (rng.next_double() - 0.5) * 10.0, 0.0, world);
      unit.y = std::clamp(unit.y + (rng.next_double() - 0.5) * 10.0, 0.0, world);
      grid.update(state.units, index);
    }

    // Periodically compare the incrementally maintained grid against a
    // fresh rebuild and the brute-force oracle.
    if (step % 10 == 0) {
      sim::SpatialHashGrid fresh;
      fresh.configure(world, world, cell, 8);
      fresh.build(state.units);
      for (std::uint32_t i = 0; i < state.units.size(); ++i) {
        if (!state.units[i].alive) {
          continue;
        }
        const int brute = sim::nearest_enemy_index(state.units, state.units[i]);
        ASSERT_EQ(grid.nearest_enemy(state.units, state.units[i]), brute)
            << "maintained grid vs brute at step " << step << " unit " << i;
        ASSERT_EQ(fresh.nearest_enemy(state.units, state.units[i]), brute)
            << "rebuilt grid vs brute at step " << step << " unit " << i;
      }
    }
  }
}

// m14 item 1: the query shadow and the slot-tracked swap-remove must keep
// radius_query exact under incremental maintenance (the simulation reads
// candidates from the shadow, so it must stay as current as the buckets),
// and the faction-scoped query must return exactly the both-faction scan's
// subsequence for its faction (four call sites rely on that identity).
TEST(SpatialHashPropertyTest, RadiusQueryExactUnderIncrementalMaintenance) {
  sim::Rng rng(0x5EED5EEDULL);
  const double world = 200.0;
  const double cell = 8.0;
  std::vector<sim::Unit> units;
  for (std::uint32_t i = 0; i < 40; ++i) {
    units.push_back(make_unit(
        i + 1,
        rng.next_uint32(2) == 0 ? sim::Faction::Red : sim::Faction::Blue,
        rng.next_double() * world, rng.next_double() * world));
  }
  sim::SpatialHashGrid grid;
  grid.configure(world, world, cell, 4);
  grid.build(units);

  const auto brute = [&](double x, double y, double radius,
                         std::vector<std::uint32_t>& expected) {
    expected.clear();
    const double radius_sq = radius * radius;
    for (std::size_t i = 0; i < units.size(); ++i) {
      if (!units[i].alive) {
        continue;
      }
      const double dx = units[i].x - x;
      const double dy = units[i].y - y;
      if (dx * dx + dy * dy <= radius_sq) {
        expected.push_back(static_cast<std::uint32_t>(i));
      }
    }
    std::sort(expected.begin(), expected.end());
  };

  std::vector<std::uint32_t> found;
  std::vector<std::uint32_t> expected;
  for (int step = 0; step < 300; ++step) {
    const std::uint32_t index =
        rng.next_uint32(static_cast<std::uint32_t>(units.size()));
    const int op = static_cast<int>(rng.next_uint32(10));
    if (op < 5) {
      // Move — only alive units (the simulation never moves the dead),
      // position change funneled through grid.update exactly like
      // update_grid_after_movement does.
      if (units[index].alive) {
        sim::Unit& unit = units[index];
        unit.x = std::clamp(unit.x + (rng.next_double() - 0.5) * 24.0, 0.0,
                            world);
        unit.y = std::clamp(unit.y + (rng.next_double() - 0.5) * 24.0, 0.0,
                            world);
        grid.update(units, index);
      }
    } else if (op < 8) {
      // Kill — flag first, then grid.remove in the same step, exactly
      // like apply_deaths.
      if (units[index].alive) {
        units[index].alive = false;
        grid.remove(units, index);
      }
    } else {
      // Revive — flag first, then grid.insert.
      if (!units[index].alive) {
        units[index].alive = true;
        grid.insert(units, index);
      }
    }

    if (step % 10 == 0) {
      // The shadow invariant the queries rely on: every record equals
      // the authoritative unit array.
      for (std::size_t i = 0; i < units.size(); ++i) {
        const auto& s = grid.shadow_of(static_cast<std::uint32_t>(i));
        ASSERT_EQ(s.x, units[i].x) << "step " << step << " unit " << i;
        ASSERT_EQ(s.y, units[i].y) << "step " << step << " unit " << i;
        ASSERT_EQ(s.id, units[i].id) << "step " << step << " unit " << i;
        ASSERT_EQ(s.faction, static_cast<std::uint8_t>(units[i].faction))
            << "step " << step << " unit " << i;
        ASSERT_EQ(s.alive, units[i].alive) << "step " << step << " unit " << i;
      }
      for (double radius : {0.0, 5.0, 16.0, 40.0, 200.0}) {
        const double x = rng.next_double() * world;
        const double y = rng.next_double() * world;
        grid.radius_query(x, y, radius, found);
        std::sort(found.begin(), found.end());
        brute(x, y, radius, expected);
        ASSERT_EQ(found, expected)
            << "step " << step << " radius " << radius;
        // The faction-scoped query returns exactly the both-faction
        // scan's subsequence for that faction — the identity the four
        // same-faction call sites rely on.
        for (sim::Faction faction : sim::kAllFactions) {
          std::vector<std::uint32_t> both;
          grid.radius_query(x, y, radius, both);
          std::vector<std::uint32_t> subsequence;
          for (std::uint32_t candidate : both) {
            if (grid.shadow_of(candidate).faction ==
                static_cast<std::uint8_t>(faction)) {
              subsequence.push_back(candidate);
            }
          }
          std::vector<std::uint32_t> one;
          grid.radius_query_faction(x, y, radius, faction, one);
          ASSERT_EQ(one, subsequence)
              << "step " << step << " radius " << radius;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Targeted edge cases.
// ---------------------------------------------------------------------------

TEST(SpatialHashEdgeTest, OccupancyBitmapStaysInLockstepWithBuckets) {
  // M15 S5: radius_query and radius_query_faction skip (cell, faction)
  // buckets the occupancy bitmap reports empty, so the bitmap must stay
  // exactly in lockstep with bucket emptiness through build/update/
  // remove/insert churn. A clear bit over a non-empty bucket would
  // silently drop candidates from every query touching that cell.
  // verify_contents (which the S1 checker runs every 30 ticks in test
  // builds) pins the lockstep invariant; this test drives it through
  // the churn directly and oracle-compares the queries at every stage.
  sim::Rng rng(0x0CC0F1A5ULL);
  const double world = 160.0;
  const double cell = 8.0;  // 20x20 cells; faction 2 never has units
  std::vector<sim::Unit> units;
  for (std::uint32_t i = 0; i < 48; ++i) {
    units.push_back(make_unit(
        i + 1,
        rng.next_uint32(2) == 0 ? sim::Faction::Red : sim::Faction::Blue,
        rng.next_double() * world, rng.next_double() * world));
  }
  sim::SpatialHashGrid grid;
  grid.configure(world, world, cell, 8, 3);
  grid.build(units);

  const auto brute = [&](double x, double y, double radius,
                         std::vector<std::uint32_t>& expected) {
    expected.clear();
    const double radius_sq = radius * radius;
    for (std::size_t i = 0; i < units.size(); ++i) {
      if (!units[i].alive) {
        continue;
      }
      const double dx = units[i].x - x;
      const double dy = units[i].y - y;
      if (dx * dx + dy * dy <= radius_sq) {
        expected.push_back(static_cast<std::uint32_t>(i));
      }
    }
    std::sort(expected.begin(), expected.end());
  };

  const double query_points[][2] = {
      {80.0, 80.0}, {8.0, 152.0}, {155.0, 5.0}};
  std::vector<std::uint32_t> found;
  std::vector<std::uint32_t> expected;
  std::vector<std::string> violations;
  const auto check_stage = [&](const char* stage) {
    violations.clear();
    EXPECT_TRUE(grid.verify_contents(units, violations))
        << stage << ": " << (violations.empty() ? "" : violations[0]);
    for (const auto& point : query_points) {
      for (double radius : {0.0, 8.0, 24.0, 160.0}) {
        grid.radius_query(point[0], point[1], radius, found);
        std::sort(found.begin(), found.end());
        brute(point[0], point[1], radius, expected);
        ASSERT_EQ(found, expected) << stage << " radius " << radius;
        // The faction query stays the both-faction subsequence —
        // including for faction 2, whose every bucket is empty.
        for (std::size_t f = 0; f < 3; ++f) {
          std::vector<std::uint32_t> both;
          grid.radius_query(point[0], point[1], radius, both);
          std::vector<std::uint32_t> subsequence;
          for (std::uint32_t candidate : both) {
            if (grid.shadow_of(candidate).faction == f) {
              subsequence.push_back(candidate);
            }
          }
          std::vector<std::uint32_t> one;
          grid.radius_query_faction(point[0], point[1], radius,
                                    static_cast<sim::Faction>(f), one);
          ASSERT_EQ(one, subsequence)
              << stage << " faction " << f << " radius " << radius;
        }
      }
    }
  };

  check_stage("after build");
  for (int step = 0; step < 200; ++step) {
    const std::uint32_t index =
        rng.next_uint32(static_cast<std::uint32_t>(units.size()));
    const int op = static_cast<int>(rng.next_uint32(10));
    if (op < 5) {
      if (units[index].alive) {
        sim::Unit& unit = units[index];
        unit.x = std::clamp(unit.x + (rng.next_double() - 0.5) * 20.0, 0.0,
                            world);
        unit.y = std::clamp(unit.y + (rng.next_double() - 0.5) * 20.0, 0.0,
                            world);
        grid.update(units, index);
      }
    } else if (op < 8) {
      if (units[index].alive) {
        units[index].alive = false;
        grid.remove(units, index);
      }
    } else if (!units[index].alive) {
      units[index].alive = true;
      grid.insert(units, index);
    }
    if (step % 25 == 24) {
      check_stage("mid-churn");
    }
  }
  check_stage("after churn");
}

TEST(SpatialHashEdgeTest, TieBreakGoesToLowestUnitId) {
  sim::SpatialHashGrid grid;
  grid.configure(100.0, 100.0, 10.0, 8);
  std::vector<sim::Unit> units = {
      make_unit(5, sim::Faction::Red, 50.0, 50.0),   // query
      make_unit(3, sim::Faction::Blue, 60.0, 50.0),  // d2 = 100, right
      make_unit(2, sim::Faction::Blue, 40.0, 50.0),  // d2 = 100, left (wins)
      make_unit(9, sim::Faction::Blue, 50.0, 60.0),  // d2 = 100, below
  };
  grid.build(units);
  EXPECT_EQ(grid.nearest_enemy(units, units[0]), 2);  // id 2 at index 2

  // Exact same position, two candidates: lower id wins.
  std::vector<sim::Unit> stacked = {
      make_unit(4, sim::Faction::Red, 10.0, 10.0),
      make_unit(8, sim::Faction::Blue, 20.0, 20.0),
      make_unit(6, sim::Faction::Blue, 20.0, 20.0),  // same spot, lower id
  };
  grid.build(stacked);
  EXPECT_EQ(grid.nearest_enemy(stacked, stacked[0]), 2);  // id 6 at index 2
}

TEST(SpatialHashEdgeTest, WorldEdgesAndCornersAreExact) {
  sim::SpatialHashGrid grid;
  grid.configure(100.0, 100.0, 10.0, 8);
  std::vector<sim::Unit> units = {
      make_unit(1, sim::Faction::Red, 0.0, 0.0),      // top-left corner
      make_unit(2, sim::Faction::Blue, 100.0, 100.0),  // bottom-right corner
      make_unit(3, sim::Faction::Blue, 0.0, 100.0),    // other corner
  };
  grid.build(units);
  EXPECT_EQ(grid.nearest_enemy(units, units[0]),
            sim::nearest_enemy_index(units, units[0]));
  EXPECT_EQ(grid.nearest_enemy(units, units[1]),
            sim::nearest_enemy_index(units, units[1]));

  // Units exactly on internal cell boundaries (x = 30 with cell 10).
  std::vector<sim::Unit> boundary = {
      make_unit(1, sim::Faction::Red, 30.0, 30.0),
      make_unit(2, sim::Faction::Blue, 30.0, 41.0),
      make_unit(3, sim::Faction::Blue, 29.999, 30.0),
  };
  grid.build(boundary);
  for (const sim::Unit& unit : boundary) {
    EXPECT_EQ(grid.nearest_enemy(boundary, unit),
              sim::nearest_enemy_index(boundary, unit));
  }
}

TEST(SpatialHashEdgeTest, DeadAndMissingFactionsHaveNoEnemy) {
  sim::SpatialHashGrid grid;
  grid.configure(100.0, 100.0, 10.0, 8);
  // Single unit, no enemy at all.
  std::vector<sim::Unit> single = {make_unit(1, sim::Faction::Red, 50.0, 50.0)};
  grid.build(single);
  EXPECT_EQ(grid.nearest_enemy(single, single[0]), -1);

  // One faction fully dead: nobody has an enemy.
  std::vector<sim::Unit> units = {
      make_unit(1, sim::Faction::Red, 10.0, 10.0),
      make_unit(2, sim::Faction::Blue, 20.0, 20.0),
  };
  units[1].alive = false;  // dead Blue stays in the vector
  grid.build(units);
  EXPECT_EQ(grid.nearest_enemy(units, units[0]), -1);
  EXPECT_EQ(sim::nearest_enemy_index(units, units[0]), -1);

  // A unit killed behind the simulation's back (alive flipped without
  // grid.remove): queries must still match the brute-force oracle — this is
  // exactly what M1's DeathTest.DeadUnitsAreNotTargeted exercises.
  std::vector<sim::Unit> poked = {
      make_unit(1, sim::Faction::Red, 0.0, 0.0),
      make_unit(2, sim::Faction::Blue, 10.0, 0.0),
      make_unit(3, sim::Faction::Blue, 40.0, 0.0),
  };
  grid.build(poked);
  poked[0].alive = false;  // out-of-band death, grid NOT updated
  for (const sim::Unit& unit : poked) {
    EXPECT_EQ(grid.nearest_enemy(poked, unit),
              sim::nearest_enemy_index(poked, unit));
  }
  // Red (dead, out-of-band) must not be targeted: with Red gone, no living
  // enemy remains for anyone.
  EXPECT_EQ(grid.nearest_enemy(poked, poked[1]), -1);
}

TEST(SpatialHashEdgeTest, ConfigureRejectsInvalidParameters) {
  sim::SpatialHashGrid grid;
  EXPECT_THROW(grid.configure(100.0, 100.0, 0.0, 8), std::invalid_argument);
  EXPECT_THROW(grid.configure(100.0, 100.0, -5.0, 8), std::invalid_argument);
  EXPECT_THROW(grid.configure(-1.0, 100.0, 8.0, 8), std::invalid_argument);
  EXPECT_THROW(grid.configure(100000.0, 100000.0, 0.05, 8),
               std::invalid_argument);  // > kMaxGridCells
  EXPECT_THROW(grid.build({}), std::invalid_argument);  // not configured

  grid.configure(1000.0, 1000.0, 32.0, 8);
  EXPECT_EQ(grid.cell_count(), 1024u);  // ceil(1000/32)^2
  EXPECT_DOUBLE_EQ(grid.cell_size(), 32.0);
}

// ---------------------------------------------------------------------------
// Mode regressions: every M1/M2 reference result is unchanged in both
// neighbor modes, and the two modes produce byte-identical exports.
// ---------------------------------------------------------------------------

TEST(NeighborModeRegressionTest, M1ReferencesUnchangedInBothModes) {
  struct Reference {
    std::uint64_t seed;
    bool has_winner;
    sim::Faction winner;
    std::uint64_t ticks;
    std::uint32_t red_survivors;
    std::uint32_t blue_survivors;
    std::uint32_t red_deaths;
    std::uint32_t blue_deaths;
  };
  const Reference references[] = {
      {42, true, sim::Faction::Blue, 456, 0, 64, 250, 186},
      {123, true, sim::Faction::Blue, 713, 0, 6, 250, 244},
      {5, false, sim::Faction::Red, 777, 0, 0, 250, 250},
      {0, true, sim::Faction::Blue, 709, 0, 26, 250, 224},
      {std::numeric_limits<std::uint64_t>::max(), true, sim::Faction::Red,
       633, 48, 0, 202, 250},
  };

  for (const sim::NeighborMode mode :
       {sim::NeighborMode::Brute, sim::NeighborMode::Grid}) {
    for (const Reference& reference : references) {
      sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy  // M1 defaults: 500 units, M1 stats
      config.seed = reference.seed;
      config.neighbor_mode = mode;
      sim::Simulation simulation(config);
      const sim::BattleResult result = simulation.run();

      ASSERT_TRUE(result.finished);
      EXPECT_EQ(result.has_winner, reference.has_winner)
          << "seed " << reference.seed << " mode " << sim::to_string(mode);
      if (reference.has_winner) {
        EXPECT_EQ(result.winner, reference.winner);
      }
      EXPECT_EQ(result.ticks_elapsed, reference.ticks);
      EXPECT_EQ(result.factions[kRed].survivors, reference.red_survivors);
      EXPECT_EQ(result.factions[kBlue].survivors, reference.blue_survivors);
      EXPECT_EQ(result.factions[kRed].deaths, reference.red_deaths);
      EXPECT_EQ(result.factions[kBlue].deaths, reference.blue_deaths);

      // Both modes must serialize the very same battle.
      sim::SimulationConfig other = config;
      other.neighbor_mode =
          mode == sim::NeighborMode::Brute ? sim::NeighborMode::Grid
                                           : sim::NeighborMode::Brute;
      sim::Simulation other_simulation(other);
      static_cast<void>(other_simulation.run());
      EXPECT_EQ(sim::export_state_json(simulation),
                sim::export_state_json(other_simulation))
          << "seed " << reference.seed << " mode " << sim::to_string(mode);
    }
  }
}

TEST(NeighborModeRegressionTest, MixedScenarioSeed42UnchangedInBothModes) {
  const std::string table_text = R"json([
  {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0, "range": 2.0, "cooldown": 5, "sight_radius": 40.0},
  {"id": "archer", "health": 70.0, "damage": 9.0, "speed": 1.6, "range": 8.0, "cooldown": 8, "sight_radius": 60.0},
  {"id": "cavalry", "health": 160.0, "damage": 20.0, "speed": 4.5, "range": 2.0, "cooldown": 7, "sight_radius": 50.0}
])json";
  const sim::UnitTypeTable types = sim::parse_unit_types(table_text);

  const auto run = [&types](sim::NeighborMode mode) {
    sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
    config.seed = 42;
    config.max_ticks = 3000;
    config.unit_types = &types;
    config.neighbor_mode = mode;
    for (const char* type_id : {"infantry", "archer", "cavalry"}) {
      const std::uint32_t index =
          static_cast<std::uint32_t>(types.index_of(type_id));
      const std::uint32_t count = std::string(type_id) == "cavalry" ? 50 : 100;
      config.composition[kRed].push_back({index, count});
      config.composition[kBlue].push_back({index, count});
    }
    config.total_units = 500;
    sim::Simulation simulation(config);
    const sim::BattleResult result = simulation.run();
    return std::make_pair(result, sim::export_state_json(simulation));
  };

  const auto [brute_result, brute_export] = run(sim::NeighborMode::Brute);
  const auto [grid_result, grid_export] = run(sim::NeighborMode::Grid);

  // The M2 reference values.
  ASSERT_TRUE(brute_result.finished);
  EXPECT_TRUE(brute_result.has_winner);
  EXPECT_EQ(brute_result.winner, sim::Faction::Blue);
  EXPECT_EQ(brute_result.ticks_elapsed, 664u);
  EXPECT_EQ(brute_result.factions[kRed].survivors, 0u);
  EXPECT_EQ(brute_result.factions[kBlue].survivors, 22u);

  EXPECT_EQ(grid_result.ticks_elapsed, brute_result.ticks_elapsed);
  EXPECT_EQ(grid_result.winner, brute_result.winner);
  EXPECT_EQ(grid_result.factions[kRed].survivors,
            brute_result.factions[kRed].survivors);
  EXPECT_EQ(grid_result.factions[kBlue].survivors,
            brute_result.factions[kBlue].survivors);
  EXPECT_EQ(grid_export, brute_export);
}

TEST(NeighborModeDeterminismTest, FiveThousandUnitsByteIdenticalInBothModes) {
  const auto run = [](sim::NeighborMode mode) {
    sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
    config.seed = 42;
    config.total_units = 5000;  // M3 cap: 20,000
    config.max_ticks = 60;
    config.battlefield_width = 250.0;  // small world: contact within 60 ticks
    config.battlefield_height = 250.0;
    config.neighbor_mode = mode;
    sim::Simulation simulation(config);
    static_cast<void>(simulation.run());
    return sim::export_state_json(simulation);
  };

  const std::string grid_a = run(sim::NeighborMode::Grid);
  const std::string grid_b = run(sim::NeighborMode::Grid);
  const std::string brute_a = run(sim::NeighborMode::Brute);
  const std::string brute_b = run(sim::NeighborMode::Brute);
  EXPECT_EQ(grid_a, grid_b);   // grid determinism
  EXPECT_EQ(brute_a, brute_b); // brute determinism
  EXPECT_EQ(grid_a, brute_a);  // cross-mode equality
}

// ---------------------------------------------------------------------------
// Steady-state allocation check.
// ---------------------------------------------------------------------------

TEST(GridAllocationTest, SteadyStateTickLoopAllocatesNothing) {
  // Two static walls in weapons range: every tick runs perception (grid
  // queries), cooldowns, combat (pending attack buffer reuse) and deaths
  // (grid removals). Units move at most 1.5 world units and never leave
  // their cells, so bucket occupancy only decreases — after the warm-up the
  // tick loop must not allocate at all.
  const double world = 1000.0;
  std::vector<sim::Unit> units;
  units.reserve(5000);
  std::uint32_t next_id = 1;
  for (std::uint32_t i = 0; i < 2500; ++i) {
    const double y = static_cast<double>(i) * (world / 2500.0);
    sim::Unit red = make_unit(next_id++, sim::Faction::Red, 496.0, y);
    red.stats = infantry_stats();
    red.stats.speed = 0.5;
    units.push_back(red);
    sim::Unit blue = make_unit(next_id++, sim::Faction::Blue, 497.5, y);
    blue.stats = infantry_stats();
    blue.stats.speed = 0.5;
    units.push_back(blue);
  }

  sim::SimulationConfig config;
  config.ai_mode = sim::AiMode::Legacy;  // M4: old tests pin legacy
  config.seed = 42;
  config.max_ticks = 1000;
  config.battlefield_width = world;
  config.battlefield_height = world;
  config.neighbor_mode = sim::NeighborMode::Grid;
  sim::Simulation simulation(config, units);

  const std::size_t* counter = allocation_counter();
  for (int i = 0; i < 10; ++i) {
    simulation.tick();  // warm-up: attack buffer high-water reached
  }
  const std::size_t before = *counter;
  for (int i = 0; i < 100; ++i) {
    simulation.tick();
  }
  EXPECT_EQ(*counter, before)
      << "the tick loop allocated " << (*counter - before)
      << " times in steady state";
}

}  // namespace
