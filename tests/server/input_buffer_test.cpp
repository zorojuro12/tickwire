#include "server/input_buffer.h"

#include <gtest/gtest.h>

namespace server {
namespace {

sim::InputCommand makeInput(uint32_t tick) {
  return sim::InputCommand{
      .player_id = 1, .tick = tick, .move_x = 0, .move_y = 0, .aim_x = 0, .aim_y = 0, .fire = false};
}

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

TEST(InputBufferTest, HighestTickTracksTheNewestAcceptedInput) {
  InputBuffer b;
  EXPECT_EQ(b.highestTick(), 0u);

  EXPECT_TRUE(b.push(sim::InputCommand{
      .player_id = 1, .tick = 3, .move_x = 0, .move_y = 0, .aim_x = 0, .aim_y = 0, .fire = false}));
  EXPECT_TRUE(b.push(sim::InputCommand{
      .player_id = 1, .tick = 7, .move_x = 0, .move_y = 0, .aim_x = 0, .aim_y = 0, .fire = false}));
  EXPECT_TRUE(b.push(sim::InputCommand{
      .player_id = 1, .tick = 5, .move_x = 0, .move_y = 0, .aim_x = 0, .aim_y = 0, .fire = false}));
  EXPECT_EQ(b.highestTick(), 7u);

  sim::InputCommand out{};
  EXPECT_TRUE(b.takeFor(7, out));
  EXPECT_EQ(b.highestTick(), 7u);
}

TEST(InputBufferTest, PushRejectsOutsideTheAcceptanceWindow) {
  InputBuffer b;

  EXPECT_FALSE(b.push(makeInput(0)));
  EXPECT_EQ(b.highestTick(), 0u);

  EXPECT_TRUE(b.push(makeInput(kInputBufferSlots)));
  EXPECT_FALSE(b.push(makeInput(kInputBufferSlots + 1)));
  EXPECT_EQ(b.highestTick(), kInputBufferSlots);

  // Nothing was pushed at tick 10, so this is a legitimate underrun (false)
  // -- the point of this call is to drive the consumption floor forward.
  sim::InputCommand out{};
  b.takeFor(10, out);
  EXPECT_EQ(b.lastConsumed(), 10u);

  EXPECT_FALSE(b.push(makeInput(10)));
  EXPECT_FALSE(b.push(makeInput(9)));
  EXPECT_TRUE(b.push(makeInput(11)));

  InputBuffer silent;
  for (uint32_t t = 1; t <= 40; ++t) {
    EXPECT_FALSE(silent.takeFor(t, out));
  }
  EXPECT_TRUE(silent.push(makeInput(45)));
}

}  // namespace
}  // namespace server
