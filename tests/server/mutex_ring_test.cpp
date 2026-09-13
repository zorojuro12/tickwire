#include "server/mutex_ring.h"

#include <cstdint>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

namespace server {
namespace {

TEST(MutexRingTest, CommittedWriteBecomesReadableInOrder) {
  MutexRing<uint32_t, 4> ring;

  EXPECT_EQ(ring.size(), 0u);
  EXPECT_EQ(ring.capacity(), 4u);
  EXPECT_EQ(ring.acquireRead(), nullptr);

  uint32_t* slot = ring.acquireWrite();
  ASSERT_NE(slot, nullptr);
  *slot = 11;

  EXPECT_EQ(ring.size(), 0u);
  EXPECT_EQ(ring.acquireRead(), nullptr);

  uint32_t* slot_again = ring.acquireWrite();
  EXPECT_EQ(slot_again, slot);

  ring.commitWrite();
  EXPECT_EQ(ring.size(), 1u);

  uint32_t* read_slot = ring.acquireRead();
  ASSERT_NE(read_slot, nullptr);
  EXPECT_EQ(*read_slot, 11u);
  ring.commitRead();
  EXPECT_EQ(ring.size(), 0u);
  EXPECT_EQ(ring.acquireRead(), nullptr);

  for (uint32_t v : {1u, 2u, 3u, 4u}) {
    uint32_t* s = ring.acquireWrite();
    ASSERT_NE(s, nullptr) << "value " << v;
    *s = v;
    ring.commitWrite();
  }
  EXPECT_EQ(ring.size(), 4u);
  EXPECT_EQ(ring.acquireWrite(), nullptr);

  ring.commitRead();
  EXPECT_NE(ring.acquireWrite(), nullptr);
}

TEST(MutexRingTest, ItemsCrossARealThreadBoundaryInOrderWithNoGapsOrDuplicates) {
  constexpr uint64_t kItems = 100000;
  auto ring = std::make_unique<MutexRing<uint64_t, 64>>();

  std::thread producer([&ring] {
    for (uint64_t v = 1; v <= kItems; ++v) {
      uint64_t* slot;
      while ((slot = ring->acquireWrite()) == nullptr) std::this_thread::yield();
      *slot = v;
      ring->commitWrite();
    }
  });

  uint64_t prev = 0;
  uint64_t observed = 0;
  std::thread consumer([&] {
    while (observed < kItems) {
      uint64_t* slot = ring->acquireRead();
      if (slot == nullptr) {
        std::this_thread::yield();
        continue;
      }
      const uint64_t v = *slot;
      EXPECT_EQ(v, prev + 1);
      prev = v;
      ++observed;
      ring->commitRead();
    }
  });

  producer.join();
  consumer.join();

  EXPECT_EQ(observed, kItems);
  EXPECT_EQ(ring->size(), 0u);
}

}  // namespace
}  // namespace server
