#include "net/snapshot_ring.h"

#include <gtest/gtest.h>

#include "sim/sim.h"

TEST(SnapshotRingTest, StoresAndFindsByTick) {
  net::SnapshotRing ring;
  EXPECT_EQ(ring.count(), 0u);
  EXPECT_EQ(ring.find(100), nullptr);
  EXPECT_EQ(ring.newest(), nullptr);

  sim::WorldSnapshot s{};
  s.tick = 100;
  s.count = 1;
  s.players[0] = sim::PlayerState{1, 5.0f, -5.0f, 1.0f, 2.0f, sim::kPlayerRadius};
  ring.store(s);

  EXPECT_EQ(ring.count(), 1u);
  const sim::WorldSnapshot* found = ring.find(100);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->tick, 100u);
  EXPECT_EQ(found->count, 1u);
  EXPECT_FLOAT_EQ(found->players[0].x, 5.0f);
  EXPECT_FLOAT_EQ(found->players[0].vy, 2.0f);
  EXPECT_EQ(ring.find(99), nullptr);
  ASSERT_NE(ring.newest(), nullptr);
  EXPECT_EQ(ring.newest()->tick, 100u);

  sim::WorldSnapshot s2{};
  s2.tick = 103;
  s2.count = 0;
  ring.store(s2);

  EXPECT_NE(ring.find(100), nullptr);
  EXPECT_NE(ring.find(103), nullptr);
  ASSERT_NE(ring.newest(), nullptr);
  EXPECT_EQ(ring.newest()->tick, 103u);
  EXPECT_EQ(ring.count(), 2u);
}

TEST(SnapshotRingTest, EvictsOldestOnceFull) {
  net::SnapshotRing ring;
  for (uint32_t tick = 1; tick <= 17; ++tick) {
    sim::WorldSnapshot s{};
    s.tick = tick;
    s.count = 0;
    ring.store(s);
  }

  EXPECT_EQ(ring.count(), net::kSnapshotRingSlots);
  EXPECT_EQ(ring.find(1), nullptr);
  EXPECT_NE(ring.find(2), nullptr);
  EXPECT_NE(ring.find(17), nullptr);
  ASSERT_NE(ring.newest(), nullptr);
  EXPECT_EQ(ring.newest()->tick, 17u);
}

TEST(SnapshotRingTest, BracketsATickBetweenTwoSnapshots) {
  net::SnapshotRing ring;
  for (uint32_t tick : {100u, 103u, 106u}) {
    sim::WorldSnapshot s{};
    s.tick = tick;
    s.count = 0;
    ring.store(s);
  }

  ASSERT_NE(ring.newestAtOrBefore(104), nullptr);
  EXPECT_EQ(ring.newestAtOrBefore(104)->tick, 103u);
  ASSERT_NE(ring.oldestAfter(104), nullptr);
  EXPECT_EQ(ring.oldestAfter(104)->tick, 106u);

  ASSERT_NE(ring.newestAtOrBefore(103), nullptr);
  EXPECT_EQ(ring.newestAtOrBefore(103)->tick, 103u);
  ASSERT_NE(ring.oldestAfter(103), nullptr);
  EXPECT_EQ(ring.oldestAfter(103)->tick, 106u);

  EXPECT_EQ(ring.newestAtOrBefore(99), nullptr);
  ASSERT_NE(ring.oldestAfter(99), nullptr);
  EXPECT_EQ(ring.oldestAfter(99)->tick, 100u);

  ASSERT_NE(ring.newestAtOrBefore(200), nullptr);
  EXPECT_EQ(ring.newestAtOrBefore(200)->tick, 106u);
  EXPECT_EQ(ring.oldestAfter(200), nullptr);
}
