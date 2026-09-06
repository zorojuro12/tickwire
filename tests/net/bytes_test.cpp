#include "net/bytes.h"

#include <array>
#include <cstring>

#include <gtest/gtest.h>

namespace net {
namespace {

TEST(ByteWriterTest, EncodesExactLittleEndianBytes) {
  std::array<std::byte, 16> buf{};
  ByteWriter w(buf);
  w.u8(0x54);
  w.u16(0x0401);
  w.u32(0x040004D2);
  w.f32(1.0f);

  const std::array<std::byte, 11> expected{
      std::byte{0x54}, std::byte{0x01}, std::byte{0x04}, std::byte{0xD2},
      std::byte{0x04}, std::byte{0x00}, std::byte{0x04}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x80}, std::byte{0x3F}};
  EXPECT_EQ(w.size(), 11u);
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(buf[i], expected[i]) << "byte " << i;
  }
}

TEST(ByteWriterTest, EncodesNegativeFloat) {
  std::array<std::byte, 4> buf{};
  ByteWriter w(buf);
  w.f32(-0.5f);

  const std::array<std::byte, 4> expected{std::byte{0x00}, std::byte{0x00},
                                           std::byte{0x00}, std::byte{0xBF}};
  EXPECT_EQ(buf, expected);
}

TEST(ByteWriterTest, OverflowingWriteTouchesNothing) {
  std::array<std::byte, 8> buf{};
  for (size_t i = 3; i < buf.size(); ++i) buf[i] = std::byte{0xEE};
  const std::array<std::byte, 8> before = buf;

  ByteWriter w(std::span<std::byte>(buf).subspan(0, 3));
  w.u32(0xFFFFFFFFu);

  EXPECT_EQ(buf, before);
  EXPECT_EQ(w.size(), 0u);
}

TEST(ByteWriterTest, OverflowIsStickyAndBlocksLaterWrites) {
  std::array<std::byte, 3> buf{};
  ByteWriter w(buf);

  EXPECT_TRUE(w.ok());
  w.u16(0x1234);
  EXPECT_TRUE(w.ok());
  EXPECT_EQ(w.size(), 2u);

  w.u32(0);
  EXPECT_FALSE(w.ok());
  EXPECT_EQ(w.size(), 2u);

  w.u8(0x7F);
  EXPECT_FALSE(w.ok());
  EXPECT_EQ(w.size(), 2u);
}

TEST(ByteReaderTest, RoundTripsByteWriterOutput) {
  std::array<std::byte, 15> buf{};
  ByteWriter w(buf);
  w.u8(0x54);
  w.u16(0xBEEF);
  w.u32(0xDEADC0DE);
  w.f32(3.5f);
  w.f32(-0.0f);
  ASSERT_TRUE(w.ok());
  ASSERT_EQ(w.size(), 15u);

  ByteReader r(buf);
  EXPECT_EQ(r.u8(), 0x54);
  EXPECT_EQ(r.u16(), 0xBEEF);
  EXPECT_EQ(r.u32(), 0xDEADC0DEu);
  EXPECT_FLOAT_EQ(r.f32(), 3.5f);

  const float neg_zero = r.f32();
  EXPECT_TRUE(r.ok());
  uint32_t neg_zero_bits = 0;
  std::memcpy(&neg_zero_bits, &neg_zero, sizeof(neg_zero_bits));
  EXPECT_EQ(neg_zero_bits, 0x80000000u);

  EXPECT_EQ(r.remaining(), 0u);
  EXPECT_TRUE(r.ok());
}

}  // namespace
}  // namespace net
