#include "server/input_buffer.h"

#include <gtest/gtest.h>

namespace server {
namespace {

TEST(InputBufferTest, TakeForReturnsTheInputStampedForThatTick) {
  InputBuffer b;
  EXPECT_TRUE(b.push(sim::InputCommand{.player_id = 1,
                                        .tick = 5,
                                        .move_x = 1.0f,
                                        .move_y = 0.0f,
                                        .aim_x = 0.0f,
                                        .aim_y = 0.0f,
                                        .fire = false}));

  sim::InputCommand out{};
  EXPECT_FALSE(b.takeFor(4, out));
  EXPECT_EQ(out.tick, 0u);
  EXPECT_EQ(out.player_id, 0u);
  EXPECT_EQ(out.move_x, 0.0f);

  EXPECT_TRUE(b.takeFor(5, out));
  EXPECT_EQ(out.tick, 5u);
  EXPECT_EQ(out.move_x, 1.0f);
  EXPECT_EQ(out.player_id, 1u);

  EXPECT_FALSE(b.takeFor(5, out));
}

}  // namespace
}  // namespace server
