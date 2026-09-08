#include "client/view.h"

#include <cmath>
#include <cstring>
#include <limits>

#include <gtest/gtest.h>

#include "sim/sim.h"

namespace client {
namespace {

uint32_t bits(float f) {
  uint32_t b = 0;
  std::memcpy(&b, &f, sizeof(b));
  return b;
}

TEST(ViewTest, WorldToScreenMapsCornersAndCentreExactly) {
  constexpr float kSide = 800.0f;

  ScreenPos centre = worldToScreen(0.0f, 0.0f, kSide, 0.0f, 0.0f);
  EXPECT_EQ(centre.x, 400.0f);
  EXPECT_EQ(centre.y, 400.0f);

  ScreenPos top_left = worldToScreen(-sim::kArenaHalf, sim::kArenaHalf, kSide, 0.0f, 0.0f);
  EXPECT_EQ(top_left.x, 0.0f);
  EXPECT_EQ(top_left.y, 0.0f);

  ScreenPos bottom_right = worldToScreen(sim::kArenaHalf, -sim::kArenaHalf, kSide, 0.0f, 0.0f);
  EXPECT_EQ(bottom_right.x, 800.0f);
  EXPECT_EQ(bottom_right.y, 800.0f);

  ScreenPos offset_centre = worldToScreen(0.0f, 0.0f, kSide, 100.0f, 50.0f);
  EXPECT_EQ(offset_centre.x, 500.0f);
  EXPECT_EQ(offset_centre.y, 450.0f);

  EXPECT_EQ(worldToScreenRadius(sim::kPlayerRadius, kSide), 4.0f);
}

TEST(ViewTest, AimFromCursorNormalizesAndRejectsDegenerateInputs) {
  float ax = 0.0f, ay = 0.0f;

  aimFromCursor(0.0f, 0.0f, 3.0f, 4.0f, ax, ay);
  EXPECT_EQ(bits(ax), bits(0.6f));
  EXPECT_EQ(bits(ay), bits(0.8f));

  aimFromCursor(1.0f, 1.0f, 1.0f, 1.0f, ax, ay);
  EXPECT_EQ(ax, 0.0f);
  EXPECT_EQ(ay, 0.0f);

  aimFromCursor(std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f, 1.0f, ax, ay);
  EXPECT_EQ(ax, 0.0f);
  EXPECT_EQ(ay, 0.0f);

  aimFromCursor(0.0f, 0.0f, std::numeric_limits<float>::infinity(), 1.0f, ax, ay);
  EXPECT_EQ(ax, 0.0f);
  EXPECT_EQ(ay, 0.0f);

  EXPECT_TRUE(std::isfinite(ax));
  EXPECT_TRUE(std::isfinite(ay));
}

}  // namespace
}  // namespace client
