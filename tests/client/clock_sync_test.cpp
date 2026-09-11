#include "client/clock_sync.h"

#include <gtest/gtest.h>

namespace client {
namespace {

TEST(ClockSyncTest, FirstObservationSeedsTheAverageAndClearsOnRead) {
  ClockSync c;
  EXPECT_FALSE(c.haveEstimate());
  EXPECT_EQ(c.takeCorrection(), 0);

  // On target: the EMA is seeded at this exact value (first observation),
  // so error is immediately 0 -- no nudge.
  c.observe(100, 103);
  EXPECT_TRUE(c.haveEstimate());
  EXPECT_EQ(c.lead(), 3);
  EXPECT_EQ(c.takeCorrection(), 0);

  // A correction, once produced, is available exactly once. Use a large
  // enough deviation to snap (exempt from the nudge cooldown below) rather
  // than a modest one, which the cooldown would otherwise gate regardless
  // of read semantics. ack_tick must stay nonzero -- 0 is the "no input
  // acknowledged yet" sentinel observe() ignores outright.
  c.observe(100, 1);
  const int32_t first_read = c.takeCorrection();
  EXPECT_NE(first_read, 0);
  EXPECT_EQ(c.takeCorrection(), 0);
}

TEST(ClockSyncTest, SnapshotWithNoAcknowledgedInputIsIgnored) {
  ClockSync c;
  c.observe(500, 0);
  EXPECT_FALSE(c.haveEstimate());
  EXPECT_EQ(c.takeCorrection(), 0);
  EXPECT_EQ(c.lead(), 0);

  c.observe(500, 503);
  EXPECT_TRUE(c.haveEstimate());
  EXPECT_EQ(c.lead(), 3);
  EXPECT_EQ(c.takeCorrection(), 0);
}

// A single modest, one-off deviation must not immediately flip the nudge --
// that is exactly the raw-signal behavior that oscillates under a delayed,
// noisy feedback loop (see clock_sync.h). Only a SUSTAINED deviation,
// accumulated across several observations, should cross the deadband.
TEST(ClockSyncTest, SustainedDeviationEventuallyNudgesSingleSampleDoesNot) {
  ClockSync c;
  c.observe(100, 103);  // seed on target
  EXPECT_EQ(c.takeCorrection(), 0);

  // One modest, single-sample deviation (lead 1, two short of target).
  c.observe(100, 101);
  EXPECT_EQ(c.takeCorrection(), 0);

  // The same deviation repeated drives the average down until it crosses
  // the deadband, at which point the nudge engages in the correct
  // direction (short lead -> advance further, +1).
  bool nudged = false;
  for (int i = 0; i < 20 && !nudged; ++i) {
    c.observe(100, 101);
    if (c.takeCorrection() != 0) nudged = true;
  }
  EXPECT_TRUE(nudged);
}

TEST(ClockSyncTest, FirstObservationLargeErrorSnapsAndIsClamped) {
  ClockSync c;
  c.observe(100, 120);  // lead 20, error +17 -- at/beyond kSnapErrorTicks
  const int32_t first_snap = c.takeCorrection();
  EXPECT_LT(first_snap, 0);  // over target -> pull back
  EXPECT_EQ(c.snaps(), 1u);

  ClockSync c2;
  c2.observe(1000, 900);  // lead -100, error -103 -- beyond the clamp
  EXPECT_EQ(c2.takeCorrection(), kMaxCorrectionTicks);
  EXPECT_EQ(c2.snaps(), 1u);
}

// A snap resets the average to the target, so a sustained large deviation
// (not a one-off) is required to snap again -- the same "don't overreact
// to a single sample" principle the deadband enforces for small errors.
TEST(ClockSyncTest, SustainedLargeDeviationSnapsAgainAfterAPriorSnap) {
  ClockSync c;
  c.observe(100, 120);  // first snap
  c.takeCorrection();
  ASSERT_EQ(c.snaps(), 1u);

  bool snapped_again = false;
  for (int i = 0; i < 20 && !snapped_again; ++i) {
    c.observe(100, 120);
    if (c.snaps() > 1u) snapped_again = true;
  }
  EXPECT_TRUE(snapped_again);
}

}  // namespace
}  // namespace client
