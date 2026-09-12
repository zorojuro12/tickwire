#include "client/interpolation.h"

#include <gtest/gtest.h>

#include "sim/sim.h"

namespace client {
namespace {

TEST(InterpolatorTest, FirstObservationSeedsTheTimelineBehindTheSnapshot) {
  Interpolator interp;
  EXPECT_FALSE(interp.haveTimeline());
  EXPECT_EQ(interp.renderTick(), 0u);

  interp.observe(100);
  EXPECT_TRUE(interp.haveTimeline());
  EXPECT_EQ(interp.renderTick(), 100u - kInterpDelayTicks);
  EXPECT_EQ(interp.snaps(), 0u);

  interp.advance();
  interp.advance();
  interp.advance();
  EXPECT_EQ(interp.renderTick(), 97u);

  Interpolator interp2;
  interp2.observe(3);
  EXPECT_EQ(interp2.renderTick(), 0u);
}

TEST(InterpolatorTest, NudgesSmallDriftAndSnapsLargeDrift) {
  Interpolator interp;

  interp.observe(100);
  EXPECT_EQ(interp.renderTick(), 94u);

  interp.advance();
  interp.advance();
  interp.advance();
  interp.observe(103);
  EXPECT_EQ(interp.renderTick(), 97u);
  EXPECT_EQ(interp.snaps(), 0u);

  interp.advance();
  interp.advance();
  interp.advance();
  interp.observe(103);
  EXPECT_EQ(interp.renderTick(), 99u);

  interp.observe(103);
  EXPECT_EQ(interp.renderTick(), 98u);

  interp.observe(200);
  EXPECT_EQ(interp.renderTick(), 194u);
  EXPECT_EQ(interp.snaps(), 1u);
}

TEST(InterpolatorTest, InterpolatesBetweenBracketingSnapshots) {
  net::SnapshotRing ring;
  {
    sim::WorldSnapshot s{};
    s.tick = 100;
    s.count = 1;
    s.players[0] = sim::PlayerState{9, 0.0f, 0.0f, 0.0f, 0.0f, sim::kPlayerRadius};
    ring.store(s);
  }
  {
    sim::WorldSnapshot s{};
    s.tick = 104;
    s.count = 1;
    s.players[0] = sim::PlayerState{9, 10.0f, -4.0f, 0.0f, 0.0f, sim::kPlayerRadius};
    ring.store(s);
  }

  Interpolator interp;
  interp.observe(108);
  ASSERT_EQ(interp.renderTick(), 102u);

  float x = 0.0f, y = 0.0f;
  ASSERT_TRUE(interp.sample(ring, 9, x, y));
  EXPECT_EQ(x, 5.0f);
  EXPECT_EQ(y, -2.0f);

  net::SnapshotRing ring2;
  {
    sim::WorldSnapshot s{};
    s.tick = 100;
    s.count = 1;
    s.players[0] = sim::PlayerState{9, 0.0f, 0.0f, 0.0f, 0.0f, sim::kPlayerRadius};
    ring2.store(s);
  }
  {
    sim::WorldSnapshot s{};
    s.tick = 103;
    s.count = 1;
    s.players[0] = sim::PlayerState{9, 3.0f, 0.0f, 0.0f, 0.0f, sim::kPlayerRadius};
    ring2.store(s);
  }

  Interpolator interp2;
  interp2.observe(107);
  ASSERT_EQ(interp2.renderTick(), 101u);

  float x2 = 0.0f, y2 = 0.0f;
  ASSERT_TRUE(interp2.sample(ring2, 9, x2, y2));
  EXPECT_NEAR(x2, 1.0f, 1e-5f);
}

TEST(InterpolatorTest, FreezesAtTheNewestSnapshotRatherThanExtrapolating) {
  net::SnapshotRing ring;
  {
    sim::WorldSnapshot s{};
    s.tick = 100;
    s.count = 1;
    s.players[0] = sim::PlayerState{9, 0.0f, 0.0f, 0.0f, 0.0f, sim::kPlayerRadius};
    ring.store(s);
  }
  {
    sim::WorldSnapshot s{};
    s.tick = 103;
    s.count = 1;
    s.players[0] = sim::PlayerState{9, 3.0f, 0.0f, 0.0f, 0.0f, sim::kPlayerRadius};
    ring.store(s);
  }

  {
    Interpolator interp;
    interp.observe(116);
    ASSERT_EQ(interp.renderTick(), 110u);
    float x = 0.0f, y = 0.0f;
    ASSERT_TRUE(interp.sample(ring, 9, x, y));
    EXPECT_EQ(x, 3.0f);
  }
  {
    Interpolator interp;
    interp.observe(101);
    ASSERT_EQ(interp.renderTick(), 95u);
    float x = 0.0f, y = 0.0f;
    ASSERT_TRUE(interp.sample(ring, 9, x, y));
    EXPECT_EQ(x, 0.0f);
  }
  {
    Interpolator interp;
    net::SnapshotRing empty_ring;
    interp.observe(200);
    float x = 0.0f, y = 0.0f;
    EXPECT_FALSE(interp.sample(empty_ring, 9, x, y));
  }
}

TEST(InterpolatorTest, HandlesPlayersAppearingAndDisappearingMidWindow) {
  net::SnapshotRing ring;
  {
    sim::WorldSnapshot s{};
    s.tick = 100;
    s.count = 1;
    s.players[0] = sim::PlayerState{8, 1.0f, 0.0f, 0.0f, 0.0f, sim::kPlayerRadius};
    ring.store(s);
  }
  {
    sim::WorldSnapshot s{};
    s.tick = 104;
    s.count = 1;
    s.players[0] = sim::PlayerState{7, 9.0f, 0.0f, 0.0f, 0.0f, sim::kPlayerRadius};
    ring.store(s);
  }

  Interpolator interp;
  interp.observe(108);
  ASSERT_EQ(interp.renderTick(), 102u);

  float x = 0.0f, y = 0.0f;
  ASSERT_TRUE(interp.sample(ring, 7, x, y));
  EXPECT_EQ(x, 9.0f);

  EXPECT_FALSE(interp.sample(ring, 8, x, y));
  EXPECT_FALSE(interp.sample(ring, 30, x, y));
}

}  // namespace
}  // namespace client
