#include <gtest/gtest.h>

#include "sim/sim.h"

TEST(Smoke, AdvanceIsDeterministic) {
  float a = sim::advance(1.0f, 2.0f);
  float b = sim::advance(1.0f, 2.0f);
  EXPECT_EQ(a, b);
}

TEST(Smoke, TickDtIsOneSixtieth) {
  EXPECT_EQ(sim::kTickDt, 1.0f / 60.0f);
}
