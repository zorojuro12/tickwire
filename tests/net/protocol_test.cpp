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

}  // namespace
}  // namespace net
