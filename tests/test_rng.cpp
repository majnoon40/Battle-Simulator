// Deterministic random generation tests.
#include "sim/rng.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

TEST(RngTest, SameSeedProducesSameSequence) {
  sim::Rng a(42);
  sim::Rng b(42);
  for (int i = 0; i < 1000; ++i) {
    ASSERT_EQ(a.next_u64(), b.next_u64());
  }
}

TEST(RngTest, SameSeedProducesSameDoubleSequence) {
  sim::Rng a(12345);
  sim::Rng b(12345);
  for (int i = 0; i < 1000; ++i) {
    ASSERT_DOUBLE_EQ(a.next_double(), b.next_double());
  }
}

TEST(RngTest, DifferentSeedsProduceDifferentSequences) {
  sim::Rng a(1);
  sim::Rng b(2);
  bool differ = false;
  for (int i = 0; i < 8; ++i) {
    if (a.next_u64() != b.next_u64()) {
      differ = true;
      break;
    }
  }
  EXPECT_TRUE(differ);
}

TEST(RngTest, SeedZeroIsValidAndDeterministic) {
  sim::Rng a(0);
  sim::Rng b(0);
  ASSERT_EQ(a.next_u64(), b.next_u64());
  EXPECT_NE(a.next_u64(), 0u);  // SplitMix64 from zero state is well mixed
}

TEST(RngTest, DoublesStayInUnitHalfOpenInterval) {
  sim::Rng rng(7);
  for (int i = 0; i < 10000; ++i) {
    const double value = rng.next_double();
    ASSERT_GE(value, 0.0);
    ASSERT_LT(value, 1.0);
  }
}

TEST(RngTest, Uint32StaysInBound) {
  sim::Rng rng(99);
  for (int i = 0; i < 10000; ++i) {
    const std::uint32_t value = rng.next_uint32(10);
    ASSERT_LT(value, 10u);
  }
  EXPECT_EQ(rng.next_uint32(1), 0u);  // bound 1 must always return 0
}

TEST(RngTest, Uint32DistributesUniformly) {
  // With a fixed seed this is a deterministic count: 10 buckets over 100000
  // draws; SplitMix64 mixes well enough that every bucket is far from empty.
  sim::Rng rng(2024);
  std::vector<std::uint64_t> buckets(10, 0);
  for (int i = 0; i < 100000; ++i) {
    buckets[rng.next_uint32(10)] += 1;
  }
  for (std::uint64_t count : buckets) {
    // Expected 10000 per bucket; reject anything wildly off.
    EXPECT_GT(count, 9000u);
    EXPECT_LT(count, 11000u);
  }
}

TEST(RngTest, DoubleMeanIsAboutHalf) {
  sim::Rng rng(31337);
  double sum = 0.0;
  constexpr int kDraws = 100000;
  for (int i = 0; i < kDraws; ++i) {
    sum += rng.next_double();
  }
  const double mean = sum / kDraws;
  EXPECT_NEAR(mean, 0.5, 0.01);
}

}  // namespace
