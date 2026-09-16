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
  const Camera cam{0.0f, 0.0f, 1.0f};

  ScreenPos centre = worldToScreen(0.0f, 0.0f, kSide, cam);
  EXPECT_EQ(centre.x, 400.0f);
  EXPECT_EQ(centre.y, 400.0f);

  ScreenPos top_left = worldToScreen(-sim::kArenaHalf, sim::kArenaHalf, kSide, cam);
  EXPECT_EQ(top_left.x, 0.0f);
  EXPECT_EQ(top_left.y, 0.0f);

  ScreenPos bottom_right = worldToScreen(sim::kArenaHalf, -sim::kArenaHalf, kSide, cam);
  EXPECT_EQ(bottom_right.x, 800.0f);
  EXPECT_EQ(bottom_right.y, 800.0f);

  EXPECT_EQ(worldToScreenRadius(sim::kPlayerRadius, kSide, 1.0f), 4.0f);
}

TEST(ViewTest, ZoomedCameraCentresOnItsTargetAndInvertsExactly) {
  constexpr float kSide = 800.0f;
  const Camera cam{-30.0f, -30.0f, 4.0f};

  ScreenPos centre = worldToScreen(-30.0f, -30.0f, kSide, cam);
  EXPECT_EQ(centre.x, 400.0f);
  EXPECT_EQ(centre.y, 400.0f);

  ScreenPos right = worldToScreen(-25.0f, -30.0f, kSide, cam);
  EXPECT_EQ(right.x, 560.0f);
  EXPECT_EQ(right.y, 400.0f);

  ScreenPos up = worldToScreen(-30.0f, -25.0f, kSide, cam);
  EXPECT_EQ(up.x, 400.0f);
  EXPECT_EQ(up.y, 240.0f);

  EXPECT_EQ(worldToScreenRadius(sim::kPlayerRadius, kSide, 4.0f), 16.0f);

  float wx = 0.0f, wy = 0.0f;
  screenToWorld(560.0f, 240.0f, kSide, cam, wx, wy);
  EXPECT_EQ(wx, -25.0f);
  EXPECT_EQ(wy, -25.0f);

  screenToWorld(0.0f, 0.0f, kSide, Camera{0.0f, 0.0f, 1.0f}, wx, wy);
  EXPECT_EQ(wx, -sim::kArenaHalf);
  EXPECT_EQ(wy, sim::kArenaHalf);
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
