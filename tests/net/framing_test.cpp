#include "net/framing.h"

#include <array>

#include <gtest/gtest.h>

#include "sim/sim.h"

namespace net {
namespace {

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

}  // namespace
}  // namespace net
