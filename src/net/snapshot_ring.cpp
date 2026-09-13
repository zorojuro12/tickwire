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

const sim::WorldSnapshot* SnapshotRing::newestAtOrBefore(uint32_t tick) const noexcept {
  const sim::WorldSnapshot* best = nullptr;
  for (size_t i = 0; i < kSnapshotRingSlots; ++i) {
    if (!filled_[i] || slots_[i].tick > tick) continue;
    if (best == nullptr || slots_[i].tick > best->tick) best = &slots_[i];
  }
  return best;
}

const sim::WorldSnapshot* SnapshotRing::oldestAfter(uint32_t tick) const noexcept {
  const sim::WorldSnapshot* best = nullptr;
  for (size_t i = 0; i < kSnapshotRingSlots; ++i) {
    if (!filled_[i] || slots_[i].tick <= tick) continue;
    if (best == nullptr || slots_[i].tick < best->tick) best = &slots_[i];
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

namespace {

const sim::PlayerState* findById(const sim::WorldSnapshot& s, uint32_t id) {
  for (uint32_t i = 0; i < s.count; ++i) {
    if (s.players[i].id == id) return &s.players[i];
  }
  return nullptr;
}

}  // namespace

bool samplePlayerAt(const SnapshotRing& ring, uint32_t player_id, uint32_t tick, float& x,
                     float& y) noexcept {
  const sim::WorldSnapshot* a = ring.newestAtOrBefore(tick);
  const sim::WorldSnapshot* b = ring.oldestAfter(tick);

  if (a != nullptr && b != nullptr) {
    const sim::PlayerState* pa = findById(*a, player_id);
    const sim::PlayerState* pb = findById(*b, player_id);

    if (pa != nullptr && pb != nullptr) {
      const float alpha =
          static_cast<float>(tick - a->tick) / static_cast<float>(b->tick - a->tick);
      x = pa->x + (pb->x - pa->x) * alpha;
      y = pa->y + (pb->y - pa->y) * alpha;
      return true;
    }
    // Joined mid-window (in b, not a): held at its first known position
    // rather than lerped from a position it never had. Departed mid-window
    // (in a, not b): gone -- must stop being drawn immediately rather than
    // lingering for the length of the window.
    if (pb != nullptr) {
      x = pb->x;
      y = pb->y;
      return true;
    }
    return false;
  }

  // Starved: no extrapolation, deliberately -- Decision 5 (freeze at the
  // newest known position, never guess a velocity-projected one that must
  // later be visibly retracted).
  if (a != nullptr) {
    const sim::PlayerState* pa = findById(*a, player_id);
    if (pa == nullptr) return false;
    x = pa->x;
    y = pa->y;
    return true;
  }
  if (b != nullptr) {
    const sim::PlayerState* pb = findById(*b, player_id);
    if (pb == nullptr) return false;
    x = pb->x;
    y = pb->y;
    return true;
  }
  return false;
}

}  // namespace net
