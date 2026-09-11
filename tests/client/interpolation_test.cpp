#include "client/interpolation.h"

#include <gtest/gtest.h>

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

}  // namespace
}  // namespace client
