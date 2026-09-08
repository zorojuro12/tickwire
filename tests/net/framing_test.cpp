#include "net/framing.h"

#include <array>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "net/protocol.h"
#include "net/transport.h"
#include "sim/sim.h"

namespace net {
namespace {

constexpr std::array<std::byte, kInputBytes> kGoldenInputPayload = {
    std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0xD2}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0x3F},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xBF},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0x3F},
    std::byte{0x01}};

TEST(JoinAcceptCodecTest, EncodesToExactBytesAndDecodesBack) {
  std::array<std::byte, kJoinAcceptBytes> buf{};
  ByteWriter w(buf);
  EXPECT_TRUE(encodeJoinAccept(7, w));
  EXPECT_EQ(w.size(), kJoinAcceptBytes);

  const std::array<std::byte, kJoinAcceptBytes> expected = {
      std::byte{0x07}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
  EXPECT_EQ(buf, expected);

  ByteReader r(buf);
  uint32_t out = 0;
  EXPECT_TRUE(decodeJoinAccept(r, out));
  EXPECT_EQ(out, 7u);
}

TEST(JoinAcceptCodecTest, RejectsFramingMismatch) {
  const std::array<std::byte, kJoinAcceptBytes> golden = {
      std::byte{0x07}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};

  {
    ByteReader r(std::span<const std::byte>(golden).subspan(0, 3));
    uint32_t out = 0xAAAAAAAAu;
    EXPECT_FALSE(decodeJoinAccept(r, out));
    EXPECT_EQ(out, 0xAAAAAAAAu);
  }
  {
    std::array<std::byte, 5> overlong{};
    for (size_t i = 0; i < golden.size(); ++i) overlong[i] = golden[i];
    overlong[4] = std::byte{0xEE};
    ByteReader r(overlong);
    uint32_t out = 0xAAAAAAAAu;
    EXPECT_FALSE(decodeJoinAccept(r, out));
    EXPECT_EQ(out, 0xAAAAAAAAu);
  }
}

TEST(JoinAcceptCodecTest, RejectsInvalidPlayerIdOnBothSides) {
  std::array<std::byte, kJoinAcceptBytes> buf{};
  ByteWriter w(buf);
  EXPECT_FALSE(encodeJoinAccept(sim::kInvalidPlayerId, w));
  EXPECT_EQ(w.size(), 0u);

  const std::array<std::byte, kJoinAcceptBytes> zero_bytes{};
  ByteReader r(zero_bytes);
  uint32_t out = 0xAAAAAAAAu;
  EXPECT_FALSE(decodeJoinAccept(r, out));
  EXPECT_EQ(out, 0xAAAAAAAAu);
}

TEST(FramePacketTest, BuildsAWholeDatagramWithACorrectPayloadLen) {
  PacketHeader h;
  h.type = MsgType::kInput;
  h.tick = 1234;
  h.send_time_ms = 123456;
  h.ack_tick = 1200;
  h.seq = 7;
  h.ack_seq = 9;
  // h.payload_len is left at its default 0, to prove framePacket sets it.

  std::array<std::byte, kMaxPacket> out{};
  const size_t written = framePacket(h, kGoldenInputPayload, out);
  EXPECT_EQ(written, kHeaderBytes + kInputBytes);

  const std::array<std::byte, kHeaderBytes> expected_header = {
      std::byte{0x54}, std::byte{0x57}, std::byte{0x49}, std::byte{0x52},
      std::byte{0x02}, std::byte{0x01}, std::byte{0x19}, std::byte{0x00},
      std::byte{0xD2}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x40}, std::byte{0xE2}, std::byte{0x01}, std::byte{0x00},
      std::byte{0xB0}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x07}, std::byte{0x00}, std::byte{0x09}, std::byte{0x00}};
  for (size_t i = 0; i < kHeaderBytes; ++i) {
    EXPECT_EQ(out[i], expected_header[i]) << "header byte " << i;
  }
  for (size_t i = 0; i < kInputBytes; ++i) {
    EXPECT_EQ(out[kHeaderBytes + i], kGoldenInputPayload[i]) << "payload byte " << i;
  }

  ByteReader r(std::span<const std::byte>(out).subspan(0, written));
  PacketHeader decoded_header;
  ASSERT_TRUE(decodeHeader(r, decoded_header));
  EXPECT_EQ(decoded_header.payload_len, kInputBytes);
  sim::InputCommand decoded_input{};
  ASSERT_TRUE(decodeInput(r, decoded_input));
  EXPECT_EQ(decoded_input.player_id, 3u);
  EXPECT_EQ(decoded_input.tick, 1234u);
  EXPECT_TRUE(decoded_input.fire);
}

TEST(FramePacketTest, EmptyPayloadFramesToExactlyTheHeader) {
  PacketHeader h;
  h.type = MsgType::kJoinRequest;

  std::array<std::byte, kMaxPacket> out{};
  const size_t written = framePacket(h, {}, out);
  EXPECT_EQ(written, kHeaderBytes);
  EXPECT_EQ(out[6], std::byte{0x00});
  EXPECT_EQ(out[7], std::byte{0x00});

  ByteReader r(std::span<const std::byte>(out).subspan(0, written));
  PacketHeader decoded_header;
  EXPECT_TRUE(decodeHeader(r, decoded_header));
  EXPECT_EQ(decoded_header.payload_len, 0u);
}

TEST(FramePacketTest, RejectionsWriteNothingDetectable) {
  PacketHeader h;
  h.type = MsgType::kInput;

  {
    std::vector<std::byte> oversized(kMaxPacket - kHeaderBytes + 1);
    std::array<std::byte, kMaxPacket> out{};
    for (auto& b : out) b = std::byte{0xEE};
    const std::array<std::byte, kMaxPacket> before = out;
    EXPECT_EQ(framePacket(h, oversized, out), 0u);
    EXPECT_EQ(out, before);
  }
  {
    std::array<std::byte, 23> out{};
    for (auto& b : out) b = std::byte{0xEE};
    const std::array<std::byte, 23> before = out;
    EXPECT_EQ(framePacket(h, {}, out), 0u);
    EXPECT_EQ(out, before);
  }
  {
    std::array<std::byte, 48> out{};
    for (auto& b : out) b = std::byte{0xEE};
    const std::array<std::byte, 48> before = out;
    EXPECT_EQ(framePacket(h, kGoldenInputPayload, out), 0u);
    EXPECT_EQ(out, before);
  }
}

}  // namespace
}  // namespace net
