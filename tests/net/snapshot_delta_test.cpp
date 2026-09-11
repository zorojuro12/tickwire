#include "net/snapshot_delta.h"

#include <array>

#include <gtest/gtest.h>

#include "net/bytes.h"
#include "net/protocol.h"
#include "sim/sim.h"

namespace net {
namespace {

TEST(SnapshotDeltaTest, UnchangedPlayerIsOmittedAndRestored) {
  sim::WorldSnapshot baseline{};
  baseline.tick = 100;
  baseline.count = 2;
  baseline.players[0] = sim::PlayerState{1, 1.0f, 2.0f, 0.0f, 0.0f, sim::kPlayerRadius};
  baseline.players[1] = sim::PlayerState{2, -3.0f, 4.0f, 0.0f, 0.0f, sim::kPlayerRadius};

  sim::WorldSnapshot current{};
  current.tick = 103;
  current.count = 2;
  current.players[0] = sim::PlayerState{1, 1.0f, 2.0f, 0.0f, 0.0f, sim::kPlayerRadius};
  current.players[1] = sim::PlayerState{2, -3.0f, 5.0f, 0.0f, 1.0f, sim::kPlayerRadius};

  std::array<std::byte, kMaxPacket> buf{};
  ByteWriter w(buf);
  ASSERT_TRUE(encodeSnapshotDelta(baseline, current, w));
  EXPECT_EQ(w.size(), kSnapshotDeltaFixedBytes + kDeltaRecordBytes);

  ByteReader r(std::span<const std::byte>(buf).subspan(0, w.size()));
  SnapshotDelta d{};
  ASSERT_TRUE(decodeSnapshotDelta(r, d));
  EXPECT_EQ(d.tick, 103u);
  EXPECT_EQ(d.baseline_tick, 100u);
  EXPECT_EQ(d.present_mask, 0b11u);
  EXPECT_EQ(d.changed_mask, 0b10u);
  EXPECT_EQ(d.record_count, 1u);

  sim::WorldSnapshot out{};
  ASSERT_TRUE(applySnapshotDelta(baseline, d, out));
  EXPECT_EQ(out.tick, 103u);
  EXPECT_EQ(out.count, 2u);
  for (uint32_t i = 0; i < out.count; ++i) {
    const sim::PlayerState& expected = current.players[i];
    const sim::PlayerState& got = out.players[i];
    EXPECT_EQ(got.id, expected.id);
    EXPECT_EQ(got.x, expected.x);
    EXPECT_EQ(got.y, expected.y);
    EXPECT_EQ(got.vx, expected.vx);
    EXPECT_EQ(got.vy, expected.vy);
    EXPECT_EQ(got.radius, expected.radius);
  }
}

}  // namespace
}  // namespace net
