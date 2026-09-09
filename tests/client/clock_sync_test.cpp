#include "client/clock_sync.h"

#include <gtest/gtest.h>

namespace client {
namespace {

TEST(ClockSyncTest, CorrectionFollowsTheLeadErrorAndClearsOnRead) {
  ClockSync c;
  EXPECT_FALSE(c.haveEstimate());
  EXPECT_EQ(c.takeCorrection(), 0);

  c.observe(100, 103);
  EXPECT_TRUE(c.haveEstimate());
  EXPECT_EQ(c.lead(), 3);
  EXPECT_EQ(c.takeCorrection(), 0);

  c.observe(100, 101);
  EXPECT_EQ(c.takeCorrection(), 1);
  EXPECT_EQ(c.takeCorrection(), 0);

  c.observe(100, 105);
  EXPECT_EQ(c.takeCorrection(), -1);
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

TEST(ClockSyncTest, LargeErrorsSnapAndAreClamped) {
  ClockSync c;

  c.observe(100, 120);
  EXPECT_EQ(c.takeCorrection(), -17);
  EXPECT_EQ(c.snaps(), 1u);

  c.observe(100, 80);
  EXPECT_EQ(c.takeCorrection(), 23);
  EXPECT_EQ(c.snaps(), 2u);

  c.observe(1000, 900);
  EXPECT_EQ(c.takeCorrection(), kMaxCorrectionTicks);
  EXPECT_EQ(c.snaps(), 3u);

  c.observe(100, 111);
  EXPECT_EQ(c.takeCorrection(), -1);
  EXPECT_EQ(c.snaps(), 3u);
}

}  // namespace
}  // namespace client
