#include "server/jitter_stats.h"

#include <gtest/gtest.h>

namespace server {
namespace {

TEST(JitterStatsTest, NearestRankPercentilesOverAscendingSamples) {
  JitterStats<128> stats;
  for (uint64_t i = 1; i <= 100; ++i) stats.record(i);

  EXPECT_EQ(stats.count(), 100u);
  EXPECT_EQ(stats.dropped(), 0u);
  EXPECT_EQ(stats.percentileNs(0.50), 50u);
  EXPECT_EQ(stats.percentileNs(0.99), 99u);
  EXPECT_EQ(stats.maxNs(), 100u);
}

TEST(JitterStatsTest, NearestRankPercentilesOverDescendingSamples) {
  JitterStats<128> stats;
  for (uint64_t i = 100; i >= 1; --i) stats.record(i);

  EXPECT_EQ(stats.count(), 100u);
  EXPECT_EQ(stats.dropped(), 0u);
  EXPECT_EQ(stats.percentileNs(0.50), 50u);
  EXPECT_EQ(stats.percentileNs(0.99), 99u);
  EXPECT_EQ(stats.maxNs(), 100u);
}

TEST(JitterStatsTest, EmptyRecorderReturnsZero) {
  JitterStats<128> stats;
  EXPECT_EQ(stats.count(), 0u);
  EXPECT_EQ(stats.percentileNs(0.50), 0u);
  EXPECT_EQ(stats.maxNs(), 0u);
}

TEST(JitterStatsTest, SamplesPastCapacityAreDroppedAndCountedNeverOverwritten) {
  JitterStats<4> stats;
  stats.record(10);
  stats.record(20);
  stats.record(30);
  stats.record(40);
  stats.record(999);
  stats.record(999);

  EXPECT_EQ(stats.count(), 4u);
  EXPECT_EQ(stats.dropped(), 2u);
  EXPECT_EQ(stats.maxNs(), 40u);
}

}  // namespace
}  // namespace server
