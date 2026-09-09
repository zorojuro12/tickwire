#include "client/prediction.h"

#include <gtest/gtest.h>

namespace client {
namespace {

sim::InputCommand makeInput(uint32_t tick, float move_x) {
  return sim::InputCommand{.player_id = 1,
                            .tick = tick,
                            .move_x = move_x,
                            .move_y = 0,
                            .aim_x = 0,
                            .aim_y = 0,
                            .fire = false};
}

TEST(PendingInputsTest, RecordsByTickAndEvictsBeyondCapacity) {
  PendingInputs p;
  EXPECT_EQ(p.find(5), nullptr);

  p.record(makeInput(5, 1.0f), 250);
  const PendingInput* got = p.find(5);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->cmd.move_x, 1.0f);
  EXPECT_EQ(got->send_time_ms, 250u);

  EXPECT_EQ(p.find(6), nullptr);

  for (uint32_t t = 5; t <= 5 + kPendingInputSlots; ++t) {
    p.record(makeInput(t, 0.0f), 0);
  }
  EXPECT_EQ(p.find(5), nullptr);
  EXPECT_NE(p.find(5 + kPendingInputSlots), nullptr);
}

}  // namespace
}  // namespace client
