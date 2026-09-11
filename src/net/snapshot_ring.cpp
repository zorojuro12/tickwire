#include "net/snapshot_ring.h"

namespace net {

void SnapshotRing::store(const sim::WorldSnapshot& s) noexcept {
  slots_[next_] = s;
  filled_[next_] = true;
  next_ = (next_ + 1) % kSnapshotRingSlots;
}

const sim::WorldSnapshot* SnapshotRing::find(uint32_t tick) const noexcept {
  for (size_t i = 0; i < kSnapshotRingSlots; ++i) {
    if (filled_[i] && slots_[i].tick == tick) return &slots_[i];
  }
  return nullptr;
}

const sim::WorldSnapshot* SnapshotRing::newest() const noexcept {
  const sim::WorldSnapshot* best = nullptr;
  for (size_t i = 0; i < kSnapshotRingSlots; ++i) {
    if (!filled_[i]) continue;
    if (best == nullptr || slots_[i].tick > best->tick) best = &slots_[i];
  }
  return best;
}

uint32_t SnapshotRing::count() const noexcept {
  uint32_t n = 0;
  for (size_t i = 0; i < kSnapshotRingSlots; ++i) {
    if (filled_[i]) ++n;
  }
  return n;
}

}  // namespace net
