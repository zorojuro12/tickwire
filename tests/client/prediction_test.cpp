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

TEST(PredictionStatsTest, ReportsPercentilesOverTheRollingWindow) {
  PredictionStats s;
  EXPECT_EQ(s.samples(), 0u);
  EXPECT_EQ(s.p50(), 0.0f);
  EXPECT_EQ(s.p99(), 0.0f);
  EXPECT_EQ(s.worst(), 0.0f);

  for (int i = 1; i <= 100; ++i) s.record(static_cast<float>(i));
  EXPECT_EQ(s.samples(), 100u);
  EXPECT_EQ(s.p50(), 50.0f);
  EXPECT_EQ(s.p99(), 99.0f);
  EXPECT_EQ(s.worst(), 100.0f);

  s.record(1000.0f);
  for (int i = 0; i < 600; ++i) s.record(1.0f);
  EXPECT_EQ(s.samples(), 701u);
  EXPECT_EQ(s.p50(), 1.0f);
  EXPECT_EQ(s.p99(), 1.0f);
  EXPECT_EQ(s.worst(), 1000.0f);
}

}  // namespace
}  // namespace client
