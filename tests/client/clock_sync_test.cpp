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

}  // namespace
}  // namespace client
