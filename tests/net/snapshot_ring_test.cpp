#include "net/snapshot_ring.h"

#include <memory>

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

TEST(SnapshotRingTest, SamplePlayerAtFollowsTheInterpolationRules) {
  auto ring = std::make_unique<net::SnapshotRing>();

  sim::WorldSnapshot s100{};
  s100.tick = 100;
  s100.count = 2;
  s100.players[0] = {.id = 2, .x = 0.0f, .y = 0.0f, .vx = 0.0f, .vy = 0.0f, .radius = sim::kPlayerRadius};
  s100.players[1] = {.id = 4, .x = 7.0f, .y = 7.0f, .vx = 0.0f, .vy = 0.0f, .radius = sim::kPlayerRadius};
  ring->store(s100);

  sim::WorldSnapshot s104{};
  s104.tick = 104;
  s104.count = 2;
  s104.players[0] = {.id = 2, .x = 10.0f, .y = -4.0f, .vx = 0.0f, .vy = 0.0f, .radius = sim::kPlayerRadius};
  s104.players[1] = {.id = 3, .x = 20.0f, .y = 20.0f, .vx = 0.0f, .vy = 0.0f, .radius = sim::kPlayerRadius};
  ring->store(s104);

  auto sample = [&](uint32_t id, uint32_t tick, float& x, float& y) {
    return net::samplePlayerAt(*ring, id, tick, x, y);
  };

  float x = -999.0f, y = -999.0f;
  EXPECT_TRUE(sample(2, 102, x, y));
  EXPECT_EQ(x, 5.0f);
  EXPECT_EQ(y, -2.0f);

  x = -999.0f, y = -999.0f;
  EXPECT_TRUE(sample(2, 100, x, y));
  EXPECT_EQ(x, 0.0f);
  EXPECT_EQ(y, 0.0f);

  x = -999.0f, y = -999.0f;
  EXPECT_TRUE(sample(2, 110, x, y));
  EXPECT_EQ(x, 10.0f);
  EXPECT_EQ(y, -4.0f);

  x = -999.0f, y = -999.0f;
  EXPECT_TRUE(sample(2, 90, x, y));
  EXPECT_EQ(x, 0.0f);
  EXPECT_EQ(y, 0.0f);

  x = -999.0f, y = -999.0f;
  EXPECT_TRUE(sample(3, 102, x, y));
  EXPECT_EQ(x, 20.0f);
  EXPECT_EQ(y, 20.0f);

  x = -111.0f, y = -111.0f;
  EXPECT_FALSE(sample(4, 102, x, y));
  EXPECT_EQ(x, -111.0f);
  EXPECT_EQ(y, -111.0f);

  x = -111.0f, y = -111.0f;
  EXPECT_FALSE(sample(4, 110, x, y));
  EXPECT_EQ(x, -111.0f);
  EXPECT_EQ(y, -111.0f);

  x = -111.0f, y = -111.0f;
  EXPECT_FALSE(sample(9, 102, x, y));
  EXPECT_EQ(x, -111.0f);
  EXPECT_EQ(y, -111.0f);

  auto empty = std::make_unique<net::SnapshotRing>();
  x = -111.0f, y = -111.0f;
  EXPECT_FALSE(net::samplePlayerAt(*empty, 2, 102, x, y));
  EXPECT_EQ(x, -111.0f);
  EXPECT_EQ(y, -111.0f);
}
