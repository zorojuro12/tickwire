#include "net/protocol.h"

#include <array>
#include <cstring>

#include <gtest/gtest.h>

namespace net {
namespace {

constexpr std::array<std::byte, kHeaderBytes> kGoldenHeaderBytes = {
    std::byte{0x54}, std::byte{0x57}, std::byte{0x49}, std::byte{0x52},
    std::byte{0x01}, std::byte{0x01}, std::byte{0x04}, std::byte{0x00},
    std::byte{0xD2}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x40}, std::byte{0xE2}, std::byte{0x01}, std::byte{0x00},
    std::byte{0xB0}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x07}, std::byte{0x00}, std::byte{0x09}, std::byte{0x00}};

PacketHeader goldenHeader() {
  PacketHeader h;
  h.magic = kProtocolMagic;
  h.version = 1;
  h.type = MsgType::kInput;
  h.payload_len = 4;
  h.tick = 1234;
  h.send_time_ms = 123456;
  h.ack_tick = 1200;
  h.seq = 7;
  h.ack_seq = 9;
  return h;
}

TEST(ProtocolHeaderTest, EncodesToExactBytesAndDecodesBack) {
  std::array<std::byte, 28> buf{};
  ByteWriter w(buf);
  ASSERT_TRUE(encodeHeader(goldenHeader(), w));
  EXPECT_EQ(w.size(), kHeaderBytes);
  for (size_t i = 0; i < kHeaderBytes; ++i) {
    EXPECT_EQ(buf[i], kGoldenHeaderBytes[i]) << "byte " << i;
  }

  buf[24] = std::byte{0xAA};
  buf[25] = std::byte{0xBB};
  buf[26] = std::byte{0xCC};
  buf[27] = std::byte{0xDD};

  ByteReader r(buf);
  PacketHeader out;
  ASSERT_TRUE(decodeHeader(r, out));
  const PacketHeader expected = goldenHeader();
  EXPECT_EQ(out.magic, expected.magic);
  EXPECT_EQ(out.version, expected.version);
  EXPECT_EQ(out.type, expected.type);
  EXPECT_EQ(out.payload_len, expected.payload_len);
  EXPECT_EQ(out.tick, expected.tick);
  EXPECT_EQ(out.send_time_ms, expected.send_time_ms);
  EXPECT_EQ(out.ack_tick, expected.ack_tick);
  EXPECT_EQ(out.seq, expected.seq);
  EXPECT_EQ(out.ack_seq, expected.ack_seq);
  EXPECT_EQ(r.remaining(), 4u);
}

TEST(ProtocolHeaderTest, ShortBufferIsRejectedAndLeavesOutputUntouched) {
  for (size_t prefix = 0; prefix < kHeaderBytes; ++prefix) {
    ByteReader r(std::span<const std::byte>(kGoldenHeaderBytes).subspan(0, prefix));
    PacketHeader out;
    out.tick = 0xAAAAAAAAu;
    EXPECT_FALSE(decodeHeader(r, out)) << "prefix " << prefix;
    EXPECT_EQ(out.tick, 0xAAAAAAAAu) << "prefix " << prefix;
  }
}

TEST(ProtocolHeaderTest, RejectsUnknownMagicVersionOrType) {
  auto mutated = [](size_t byte_index, uint8_t value) {
    std::array<std::byte, kHeaderBytes> bytes = kGoldenHeaderBytes;
    bytes[byte_index] = std::byte{value};
    return bytes;
  };

  const std::array<std::array<std::byte, kHeaderBytes>, 5> cases = {
      mutated(0, 0x55),  // wrong magic
      mutated(4, 0x02),  // wrong version
      mutated(5, 0x00),  // MsgType::kInvalid
      mutated(5, 0x06),  // one past kMaxMsgType
      mutated(5, 0xFF),
  };

  for (size_t i = 0; i < cases.size(); ++i) {
    ByteReader r(cases[i]);
    PacketHeader out;
    out.tick = 0xAAAAAAAAu;
    EXPECT_FALSE(decodeHeader(r, out)) << "case " << i;
    EXPECT_EQ(out.tick, 0xAAAAAAAAu) << "case " << i;
  }

  // A bare 24-byte header claiming payload_len == 4 has no payload
  // following it, so it is itself a length-confusion case rejected by
  // Checkpoint 3 below; a well-formed packet needs the payload bytes too.
  std::array<std::byte, 28> golden28{};
  for (size_t i = 0; i < kHeaderBytes; ++i) golden28[i] = kGoldenHeaderBytes[i];
  golden28[24] = std::byte{0xAA};
  golden28[25] = std::byte{0xBB};
  golden28[26] = std::byte{0xCC};
  golden28[27] = std::byte{0xDD};
  ByteReader r(golden28);
  PacketHeader out;
  EXPECT_TRUE(decodeHeader(r, out));
}

TEST(ProtocolHeaderTest, RejectsPayloadLenThatDisagreesWithThePacket) {
  std::array<std::byte, 28> golden28{};
  for (size_t i = 0; i < kHeaderBytes; ++i) golden28[i] = kGoldenHeaderBytes[i];
  golden28[24] = std::byte{0xAA};
  golden28[25] = std::byte{0xBB};
  golden28[26] = std::byte{0xCC};
  golden28[27] = std::byte{0xDD};

  auto withPayloadLen = [&](uint16_t len) {
    std::array<std::byte, 28> bytes = golden28;
    bytes[6] = std::byte(len & 0xFFu);
    bytes[7] = std::byte((len >> 8) & 0xFFu);
    return bytes;
  };

  {
    const std::array<std::byte, 28> bytes = withPayloadLen(3);
    ByteReader r(bytes);
    PacketHeader out;
    out.tick = 0xAAAAAAAAu;
    EXPECT_FALSE(decodeHeader(r, out));
    EXPECT_EQ(out.tick, 0xAAAAAAAAu);
  }
  {
    const std::array<std::byte, 28> bytes = withPayloadLen(5);
    ByteReader r(bytes);
    PacketHeader out;
    out.tick = 0xAAAAAAAAu;
    EXPECT_FALSE(decodeHeader(r, out));
    EXPECT_EQ(out.tick, 0xAAAAAAAAu);
  }
  {
    ByteReader r(std::span<const std::byte>(golden28).subspan(0, kHeaderBytes));
    PacketHeader out;
    out.tick = 0xAAAAAAAAu;
    EXPECT_FALSE(decodeHeader(r, out));
    EXPECT_EQ(out.tick, 0xAAAAAAAAu);
  }
  {
    const std::array<std::byte, 28> bytes = withPayloadLen(0xFFFF);
    ByteReader r(bytes);
    PacketHeader out;
    out.tick = 0xAAAAAAAAu;
    EXPECT_FALSE(decodeHeader(r, out));
    EXPECT_EQ(out.tick, 0xAAAAAAAAu);
  }

  ByteReader r(golden28);
  PacketHeader out;
  EXPECT_TRUE(decodeHeader(r, out));
}

TEST(InputCommandCodecTest, EncodesToExactBytesAndDecodesBack) {
  const sim::InputCommand in{
      .player_id = 3, .tick = 1234, .move_x = 1.0f, .move_y = -0.5f, .fire = true};

  std::array<std::byte, kInputBytes> buf{};
  ByteWriter w(buf);
  ASSERT_TRUE(encodeInput(in, w));
  EXPECT_EQ(w.size(), kInputBytes);

  const std::array<std::byte, kInputBytes> expected = {
      std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0xD2}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0x3F},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xBF},
      std::byte{0x01}};
  EXPECT_EQ(buf, expected);

  ByteReader r(buf);
  sim::InputCommand out{};
  ASSERT_TRUE(decodeInput(r, out));
  EXPECT_EQ(out.player_id, in.player_id);
  EXPECT_EQ(out.tick, in.tick);
  uint32_t move_x_bits = 0, expected_move_x_bits = 0;
  std::memcpy(&move_x_bits, &out.move_x, sizeof(move_x_bits));
  std::memcpy(&expected_move_x_bits, &in.move_x, sizeof(expected_move_x_bits));
  EXPECT_EQ(move_x_bits, expected_move_x_bits);
  uint32_t move_y_bits = 0, expected_move_y_bits = 0;
  std::memcpy(&move_y_bits, &out.move_y, sizeof(move_y_bits));
  std::memcpy(&expected_move_y_bits, &in.move_y, sizeof(expected_move_y_bits));
  EXPECT_EQ(move_y_bits, expected_move_y_bits);
  EXPECT_EQ(out.fire, in.fire);
}

TEST(InputCommandCodecTest, FireIsLenientToAnyNonzeroByte) {
  std::array<std::byte, kInputBytes> buf = {
      std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0xD2}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0x3F},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xBF},
      std::byte{0x00}};

  {
    ByteReader r(buf);
    sim::InputCommand out{};
    ASSERT_TRUE(decodeInput(r, out));
    EXPECT_FALSE(out.fire);
  }

  buf[16] = std::byte{0x7F};
  {
    ByteReader r(buf);
    sim::InputCommand out{};
    ASSERT_TRUE(decodeInput(r, out));
    EXPECT_TRUE(out.fire);
  }
}

TEST(InputCommandCodecTest, RejectsFramingMismatch) {
  const std::array<std::byte, kInputBytes> golden = {
      std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0xD2}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0x3F},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xBF},
      std::byte{0x01}};

  for (size_t prefix = 0; prefix < kInputBytes; ++prefix) {
    ByteReader r(std::span<const std::byte>(golden).subspan(0, prefix));
    sim::InputCommand out{};
    out.player_id = 0xAAAAAAAAu;
    EXPECT_FALSE(decodeInput(r, out)) << "prefix " << prefix;
    EXPECT_EQ(out.player_id, 0xAAAAAAAAu) << "prefix " << prefix;
  }

  std::array<std::byte, kInputBytes + 1> overlong{};
  for (size_t i = 0; i < kInputBytes; ++i) overlong[i] = golden[i];
  overlong[kInputBytes] = std::byte{0xEE};
  {
    ByteReader r(overlong);
    sim::InputCommand out{};
    out.player_id = 0xAAAAAAAAu;
    EXPECT_FALSE(decodeInput(r, out));
    EXPECT_EQ(out.player_id, 0xAAAAAAAAu);
  }
}

}  // namespace
}  // namespace net
