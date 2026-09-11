#include "server/packet_ring.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace server {
namespace {

TEST(PacketRingTest, CommittedWriteBecomesReadableInOrder) {
  PacketRing<uint32_t, 4> ring;

  EXPECT_EQ(ring.size(), 0u);
  EXPECT_EQ(ring.capacity(), 4u);
  EXPECT_EQ(ring.acquireRead(), nullptr);

  uint32_t* slot = ring.acquireWrite();
  ASSERT_NE(slot, nullptr);
  *slot = 11;

  // Uncommitted write is not visible.
  EXPECT_EQ(ring.size(), 0u);
  EXPECT_EQ(ring.acquireRead(), nullptr);

  // acquireWrite twice without a commit returns the same slot.
  uint32_t* slot_again = ring.acquireWrite();
  EXPECT_EQ(slot_again, slot);

  ring.commitWrite();
  EXPECT_EQ(ring.size(), 1u);

  uint32_t* read_slot = ring.acquireRead();
  ASSERT_NE(read_slot, nullptr);
  EXPECT_EQ(*read_slot, 11u);
  EXPECT_EQ(ring.size(), 1u);
  ring.commitRead();
  EXPECT_EQ(ring.size(), 0u);
  EXPECT_EQ(ring.acquireRead(), nullptr);

  // FIFO over three values.
  for (uint32_t v : {11u, 22u, 33u}) {
    uint32_t* s = ring.acquireWrite();
    ASSERT_NE(s, nullptr);
    *s = v;
    ring.commitWrite();
  }
  for (uint32_t expected : {11u, 22u, 33u}) {
    uint32_t* s = ring.acquireRead();
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, expected);
    ring.commitRead();
  }
}

TEST(PacketRingTest, RingIsBoundedAndIndicesWrapCorrectly) {
  PacketRing<uint32_t, 4> ring;

  for (uint32_t v : {1u, 2u, 3u, 4u}) {
    uint32_t* s = ring.acquireWrite();
    ASSERT_NE(s, nullptr) << "value " << v;
    *s = v;
    ring.commitWrite();
  }
  EXPECT_EQ(ring.size(), 4u);
  EXPECT_EQ(ring.acquireWrite(), nullptr);
  EXPECT_EQ(ring.size(), 4u);

  for (uint32_t expected : {1u, 2u, 3u, 4u}) {
    uint32_t* s = ring.acquireRead();
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, expected);
    ring.commitRead();
  }

  // Wrap: 100 further write/read cycles crossing the mask boundary many times.
  for (uint32_t i = 0; i < 100; ++i) {
    uint32_t value = 100 + i;
    uint32_t* w = ring.acquireWrite();
    ASSERT_NE(w, nullptr) << "cycle " << i;
    *w = value;
    ring.commitWrite();
    uint32_t* r = ring.acquireRead();
    ASSERT_NE(r, nullptr) << "cycle " << i;
    EXPECT_EQ(*r, value) << "cycle " << i;
    ring.commitRead();
  }

  // Interleaved: fill to 4, drain 2, write 2 more, then drain all 4.
  for (uint32_t v : {1u, 2u, 3u, 4u}) {
    uint32_t* s = ring.acquireWrite();
    ASSERT_NE(s, nullptr) << "value " << v;
    *s = v;
    ring.commitWrite();
  }
  EXPECT_EQ(ring.size(), 4u);
  for (int i = 0; i < 2; ++i) {
    ASSERT_NE(ring.acquireRead(), nullptr);
    ring.commitRead();
  }
  for (uint32_t v : {5u, 6u}) {
    uint32_t* s = ring.acquireWrite();
    ASSERT_NE(s, nullptr) << "value " << v;
    *s = v;
    ring.commitWrite();
  }
  EXPECT_EQ(ring.size(), 4u);
  for (uint32_t expected : {3u, 4u, 5u, 6u}) {
    uint32_t* s = ring.acquireRead();
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, expected);
    ring.commitRead();
  }
}

}  // namespace
}  // namespace server
