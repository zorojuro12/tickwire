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

}  // namespace
}  // namespace server
