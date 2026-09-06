#include <array>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "net/protocol.h"
#include "net/transport.h"
#include "sim/sim.h"

namespace net {
namespace {

std::vector<std::byte> buildInputPacket() {
  const sim::InputCommand in{
      .player_id = 3, .tick = 1234, .move_x = 1.0f, .move_y = -0.5f, .fire = true};
  std::vector<std::byte> buf(kHeaderBytes + kInputBytes);
  ByteWriter w(buf);
  PacketHeader h;
  h.type = MsgType::kInput;
  h.payload_len = static_cast<uint16_t>(kInputBytes);
  EXPECT_TRUE(encodeHeader(h, w));
  EXPECT_TRUE(encodeInput(in, w));
  EXPECT_EQ(w.size(), buf.size());
  return buf;
}

std::vector<std::byte> buildTwoPlayerSnapshotPacket() {
  sim::WorldSnapshot s{};
  s.tick = 1234;
  s.count = 2;
  s.players[0] = {.id = 1, .x = 2.0f, .y = 3.0f, .vx = 0.0f, .vy = -1.0f, .radius = 0.5f};
  s.players[1] = {.id = 2, .x = -4.0f, .y = 0.0f, .vx = 1.5f, .vy = 0.0f, .radius = 0.5f};

  const size_t payload_len = kSnapshotFixedBytes + 2 * kPlayerStateBytes;
  std::vector<std::byte> buf(kHeaderBytes + payload_len);
  ByteWriter w(buf);
  PacketHeader h;
  h.type = MsgType::kSnapshot;
  h.payload_len = static_cast<uint16_t>(payload_len);
  EXPECT_TRUE(encodeHeader(h, w));
  EXPECT_TRUE(encodeSnapshot(s, w));
  EXPECT_EQ(w.size(), buf.size());
  return buf;
}

std::vector<std::byte> buildFullSnapshotPacket() {
  sim::WorldSnapshot s{};
  s.tick = 999;
  s.count = sim::kMaxPlayers;
  for (uint32_t i = 0; i < sim::kMaxPlayers; ++i) {
    s.players[i] = {.id = i,
                     .x = static_cast<float>(i),
                     .y = static_cast<float>(i) * 2.0f,
                     .vx = 0.1f,
                     .vy = -0.1f,
                     .radius = 0.5f};
  }

  const size_t payload_len = kSnapshotFixedBytes + sim::kMaxPlayers * kPlayerStateBytes;
  std::vector<std::byte> buf(kHeaderBytes + payload_len);
  ByteWriter w(buf);
  PacketHeader h;
  h.type = MsgType::kSnapshot;
  h.payload_len = static_cast<uint16_t>(payload_len);
  EXPECT_TRUE(encodeHeader(h, w));
  EXPECT_TRUE(encodeSnapshot(s, w));
  EXPECT_EQ(w.size(), buf.size());
  EXPECT_LE(buf.size(), kMaxPacket);
  EXPECT_EQ(buf.size(), 800u);
  return buf;
}

// Decodes a header, then (on success) the InputCommand payload, from a
// prefix of `full`. Returns true only if both decode steps succeed.
bool decodeInputPacket(std::span<const std::byte> bytes, PacketHeader& header_out,
                        sim::InputCommand& payload_out) {
  ByteReader r(bytes);
  if (!decodeHeader(r, header_out)) return false;
  return decodeInput(r, payload_out);
}

bool decodeSnapshotPacket(std::span<const std::byte> bytes, PacketHeader& header_out,
                           sim::WorldSnapshot& payload_out) {
  ByteReader r(bytes);
  if (!decodeHeader(r, header_out)) return false;
  return decodeSnapshot(r, payload_out);
}

TEST(RobustnessTest, EveryTruncationOfAnInputPacketIsRejectedWithoutCrashing) {
  const std::vector<std::byte> full = buildInputPacket();

  {
    PacketHeader h;
    sim::InputCommand payload;
    EXPECT_TRUE(decodeInputPacket(full, h, payload));
  }

  for (size_t prefix = 0; prefix < full.size(); ++prefix) {
    PacketHeader h;
    h.tick = 0xAAAAAAAAu;
    sim::InputCommand payload{};
    payload.player_id = 0xAAAAAAAAu;

    const bool ok = decodeInputPacket(std::span<const std::byte>(full).subspan(0, prefix), h, payload);
    EXPECT_FALSE(ok) << "prefix " << prefix;
    EXPECT_EQ(h.tick, 0xAAAAAAAAu) << "prefix " << prefix;
    EXPECT_EQ(payload.player_id, 0xAAAAAAAAu) << "prefix " << prefix;
  }
}

TEST(RobustnessTest, EveryTruncationOfATwoPlayerSnapshotIsRejectedWithoutCrashing) {
  const std::vector<std::byte> full = buildTwoPlayerSnapshotPacket();

  {
    PacketHeader h;
    sim::WorldSnapshot payload;
    EXPECT_TRUE(decodeSnapshotPacket(full, h, payload));
  }

  for (size_t prefix = 0; prefix < full.size(); ++prefix) {
    PacketHeader h;
    h.tick = 0xAAAAAAAAu;
    sim::WorldSnapshot payload{};
    payload.tick = 0xAAAAAAAAu;

    const bool ok =
        decodeSnapshotPacket(std::span<const std::byte>(full).subspan(0, prefix), h, payload);
    EXPECT_FALSE(ok) << "prefix " << prefix;
    EXPECT_EQ(h.tick, 0xAAAAAAAAu) << "prefix " << prefix;
    EXPECT_EQ(payload.tick, 0xAAAAAAAAu) << "prefix " << prefix;
  }
}

TEST(RobustnessTest, EveryTruncationOfAFullSnapshotIsRejectedWithoutCrashing) {
  const std::vector<std::byte> full = buildFullSnapshotPacket();

  {
    PacketHeader h;
    sim::WorldSnapshot payload;
    EXPECT_TRUE(decodeSnapshotPacket(full, h, payload));
  }

  for (size_t prefix = 0; prefix < full.size(); ++prefix) {
    PacketHeader h;
    h.tick = 0xAAAAAAAAu;
    sim::WorldSnapshot payload{};
    payload.tick = 0xAAAAAAAAu;

    const bool ok =
        decodeSnapshotPacket(std::span<const std::byte>(full).subspan(0, prefix), h, payload);
    EXPECT_FALSE(ok) << "prefix " << prefix;
    EXPECT_EQ(h.tick, 0xAAAAAAAAu) << "prefix " << prefix;
    EXPECT_EQ(payload.tick, 0xAAAAAAAAu) << "prefix " << prefix;
  }
}

}  // namespace
}  // namespace net
