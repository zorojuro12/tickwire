#include "net/snapshot_delta.h"

#include <array>
#include <cmath>
#include <limits>

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

TEST(SnapshotDeltaTest, DepartedPlayerIsDroppedFromPresentMask) {
  sim::WorldSnapshot baseline{};
  baseline.tick = 100;
  baseline.count = 3;
  baseline.players[0] = sim::PlayerState{1, 0.0f, 0.0f, 0.0f, 0.0f, sim::kPlayerRadius};
  baseline.players[1] = sim::PlayerState{2, 1.0f, 1.0f, 0.0f, 0.0f, sim::kPlayerRadius};
  baseline.players[2] = sim::PlayerState{3, 2.0f, 2.0f, 0.0f, 0.0f, sim::kPlayerRadius};

  sim::WorldSnapshot current{};
  current.tick = 103;
  current.count = 2;
  current.players[0] = sim::PlayerState{1, 0.0f, 0.0f, 0.0f, 0.0f, sim::kPlayerRadius};
  current.players[1] = sim::PlayerState{3, 2.0f, 2.0f, 0.0f, 0.0f, sim::kPlayerRadius};

  std::array<std::byte, kMaxPacket> buf{};
  ByteWriter w(buf);
  ASSERT_TRUE(encodeSnapshotDelta(baseline, current, w));
  EXPECT_EQ(w.size(), kSnapshotDeltaFixedBytes);

  ByteReader r(std::span<const std::byte>(buf).subspan(0, w.size()));
  SnapshotDelta d{};
  ASSERT_TRUE(decodeSnapshotDelta(r, d));
  EXPECT_EQ(d.present_mask, 0b101u);
  EXPECT_EQ(d.changed_mask, 0u);
  EXPECT_EQ(d.record_count, 0u);

  sim::WorldSnapshot out{};
  ASSERT_TRUE(applySnapshotDelta(baseline, d, out));
  EXPECT_EQ(out.count, 2u);
  EXPECT_EQ(out.players[0].id, 1u);
  EXPECT_EQ(out.players[1].id, 3u);
  for (uint32_t i = 0; i < out.count; ++i) {
    EXPECT_NE(out.players[i].id, 2u);
  }
}

TEST(SnapshotDeltaTest, PlayerAbsentFromBaselineIsSentInFull) {
  sim::WorldSnapshot baseline{};
  baseline.tick = 100;
  baseline.count = 1;
  baseline.players[0] = sim::PlayerState{1, 0.0f, 0.0f, 0.0f, 0.0f, sim::kPlayerRadius};

  sim::WorldSnapshot current{};
  current.tick = 103;
  current.count = 2;
  current.players[0] = sim::PlayerState{1, 0.0f, 0.0f, 0.0f, 0.0f, sim::kPlayerRadius};
  current.players[1] = sim::PlayerState{5, -35.0f, 25.0f, 0.0f, 0.0f, sim::kPlayerRadius};

  std::array<std::byte, kMaxPacket> buf{};
  ByteWriter w(buf);
  ASSERT_TRUE(encodeSnapshotDelta(baseline, current, w));
  EXPECT_EQ(w.size(), kSnapshotDeltaFixedBytes + kDeltaRecordBytes);

  ByteReader r(std::span<const std::byte>(buf).subspan(0, w.size()));
  SnapshotDelta d{};
  ASSERT_TRUE(decodeSnapshotDelta(r, d));
  EXPECT_EQ(d.present_mask, 0b10001u);
  EXPECT_EQ(d.changed_mask, 0b10000u);
  EXPECT_EQ(d.record_count, 1u);
  EXPECT_EQ(d.records[0].id, 5u);

  sim::WorldSnapshot out{};
  ASSERT_TRUE(applySnapshotDelta(baseline, d, out));
  EXPECT_EQ(out.count, 2u);
  bool found5 = false;
  for (uint32_t i = 0; i < out.count; ++i) {
    if (out.players[i].id == 5u) {
      found5 = true;
      EXPECT_FLOAT_EQ(out.players[i].x, -35.0f);
      EXPECT_FLOAT_EQ(out.players[i].y, 25.0f);
    }
  }
  EXPECT_TRUE(found5);
}

TEST(SnapshotDeltaTest, DeltaThenApplyEqualsTheFullSnapshot) {
  sim::WorldSnapshot baseline{};
  baseline.tick = 100;
  baseline.count = sim::kMaxPlayers;
  for (uint32_t i = 0; i < sim::kMaxPlayers; ++i) {
    baseline.players[i] = sim::PlayerState{
        i + 1, static_cast<float>(i) * 1.5f, -static_cast<float>(i) * 0.25f, 0.0f, 0.0f,
        sim::kPlayerRadius};
  }

  sim::WorldSnapshot current = baseline;
  current.tick = 103;
  for (uint32_t id : {1u, 7u, 8u, 31u, 32u}) {
    sim::PlayerState& p = current.players[id - 1];
    p.x += 0.125f;
    p.vx = sim::kMoveSpeed;
  }

  std::array<std::byte, kMaxPacket> delta_buf{};
  ByteWriter dw(delta_buf);
  ASSERT_TRUE(encodeSnapshotDelta(baseline, current, dw));

  ByteReader dr(std::span<const std::byte>(delta_buf).subspan(0, dw.size()));
  SnapshotDelta d{};
  ASSERT_TRUE(decodeSnapshotDelta(dr, d));

  sim::WorldSnapshot out{};
  ASSERT_TRUE(applySnapshotDelta(baseline, d, out));
  EXPECT_EQ(out.tick, current.tick);
  EXPECT_EQ(out.count, current.count);
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

  std::array<std::byte, kMaxPacket> full_buf{};
  ByteWriter fw(full_buf);
  ASSERT_TRUE(encodeSnapshot(current, fw));
  EXPECT_LT(dw.size(), fw.size());
  EXPECT_EQ(dw.size(), kSnapshotDeltaFixedBytes + 5 * kDeltaRecordBytes);
  EXPECT_EQ(fw.size(), kSnapshotFixedBytes + sim::kMaxPlayers * kPlayerStateBytes);
}

TEST(SnapshotDeltaTest, RejectsChangedMaskNotSubsetOfPresentMask) {
  std::array<std::byte, kMaxPacket> buf{};
  ByteWriter w(buf);
  w.u32(103);              // tick
  w.u32(100);              // baseline_tick
  w.u32(0b0001);           // present_mask
  w.u32(0b0011);           // changed_mask -- bit 1 set but not present
  for (int rec = 0; rec < 2; ++rec) {
    w.f32(1.0f);
    w.f32(2.0f);
    w.f32(0.0f);
    w.f32(0.0f);
    w.f32(sim::kPlayerRadius);
  }
  ASSERT_TRUE(w.ok());

  ByteReader r(std::span<const std::byte>(buf).subspan(0, w.size()));
  SnapshotDelta d{};
  d.tick = 0xDEADBEEF;
  EXPECT_FALSE(decodeSnapshotDelta(r, d));
  EXPECT_EQ(d.tick, 0xDEADBEEFu);
}

TEST(SnapshotDeltaTest, RejectsPayloadLengthDisagreeingWithChangedMask) {
  auto buildFixed = [](uint32_t num_records) {
    std::array<std::byte, kMaxPacket> buf{};
    ByteWriter w(buf);
    w.u32(103);
    w.u32(100);
    w.u32(0b11);
    w.u32(0b11);
    for (uint32_t rec = 0; rec < num_records; ++rec) {
      w.f32(1.0f);
      w.f32(2.0f);
      w.f32(0.0f);
      w.f32(0.0f);
      w.f32(sim::kPlayerRadius);
    }
    return std::pair{buf, w.size()};
  };

  {
    // too few: one record instead of two
    auto [buf, size] = buildFixed(1);
    ByteReader r(std::span<const std::byte>(buf).subspan(0, size));
    SnapshotDelta d{};
    EXPECT_FALSE(decodeSnapshotDelta(r, d));
  }
  {
    // too many: three records instead of two -- trailing bytes
    auto [buf, size] = buildFixed(3);
    ByteReader r(std::span<const std::byte>(buf).subspan(0, size));
    SnapshotDelta d{};
    EXPECT_FALSE(decodeSnapshotDelta(r, d));
  }
}

TEST(SnapshotDeltaTest, RejectsNonFiniteRecordFields) {
  auto buildWithField = [](int field_index, float value) {
    std::array<std::byte, kMaxPacket> buf{};
    ByteWriter w(buf);
    w.u32(103);
    w.u32(100);
    w.u32(0b1);
    w.u32(0b1);
    float fields[5] = {1.0f, 2.0f, 0.0f, 0.0f, sim::kPlayerRadius};
    fields[field_index] = value;
    for (float f : fields) w.f32(f);
    return std::pair{buf, w.size()};
  };

  const float bad_values[] = {std::nanf(""), std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()};

  for (int field = 0; field < 5; ++field) {
    for (float bad : bad_values) {
      auto [buf, size] = buildWithField(field, bad);
      ByteReader r(std::span<const std::byte>(buf).subspan(0, size));
      SnapshotDelta d{};
      EXPECT_FALSE(decodeSnapshotDelta(r, d)) << "field " << field << " value " << bad;
    }
  }
}

TEST(SnapshotDeltaTest, GoldenByteVector) {
  sim::WorldSnapshot baseline{};
  baseline.tick = 0x64;
  baseline.count = 2;
  baseline.players[0] = sim::PlayerState{1, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f};
  baseline.players[1] = sim::PlayerState{2, 1.0f, 0.0f, 0.0f, 0.0f, 0.5f};

  sim::WorldSnapshot current{};
  current.tick = 0x67;
  current.count = 2;
  current.players[0] = sim::PlayerState{1, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f};
  current.players[1] = sim::PlayerState{2, 2.0f, 0.0f, 0.0f, 0.0f, 0.5f};

  std::array<std::byte, kMaxPacket> buf{};
  ByteWriter w(buf);
  ASSERT_TRUE(encodeSnapshotDelta(baseline, current, w));
  ASSERT_EQ(w.size(), 36u);

  const std::array<uint8_t, 36> expected = {
      0x67, 0x00, 0x00, 0x00,  // tick = 103
      0x64, 0x00, 0x00, 0x00,  // baseline_tick = 100
      0x03, 0x00, 0x00, 0x00,  // present_mask = 0b11
      0x02, 0x00, 0x00, 0x00,  // changed_mask = 0b10
      0x00, 0x00, 0x00, 0x40,  // x = 2.0f
      0x00, 0x00, 0x00, 0x00,  // y = 0.0f
      0x00, 0x00, 0x00, 0x00,  // vx = 0.0f
      0x00, 0x00, 0x00, 0x00,  // vy = 0.0f
      0x00, 0x00, 0x00, 0x3F,  // radius = 0.5f
  };
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(static_cast<uint8_t>(buf[i]), expected[i]) << "byte " << i;
  }

  ByteReader r(std::span<const std::byte>(buf).subspan(0, w.size()));
  SnapshotDelta d{};
  ASSERT_TRUE(decodeSnapshotDelta(r, d));
  EXPECT_EQ(d.tick, 103u);
  EXPECT_EQ(d.changed_mask, 0b10u);
  EXPECT_EQ(d.records[0].id, 2u);
  EXPECT_FLOAT_EQ(d.records[0].x, 2.0f);
}

}  // namespace
}  // namespace net
