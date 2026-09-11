#pragma once

#include <array>
#include <cstdint>

#include "sim/sim.h"

namespace net {

inline constexpr size_t kSnapshotRingSlots = 16;  // 16 snapshots at 20 Hz = 800 ms

// The most recent kSnapshotRingSlots snapshots, found by tick. Fixed storage,
// no allocation; a 16-entry linear scan, matching SessionTable's own approach.
// Callers must not store the same tick twice -- both call sites are already
// guarded by a strictly-newer check.
class SnapshotRing {
 public:
  void store(const sim::WorldSnapshot& s) noexcept;
  const sim::WorldSnapshot* find(uint32_t tick) const noexcept;          // nullptr when absent
  const sim::WorldSnapshot* newest() const noexcept;                     // nullptr when empty
  const sim::WorldSnapshot* newestAtOrBefore(uint32_t tick) const noexcept;
  const sim::WorldSnapshot* oldestAfter(uint32_t tick) const noexcept;
  uint32_t count() const noexcept;  // live entries, saturating at kSnapshotRingSlots

 private:
  std::array<sim::WorldSnapshot, kSnapshotRingSlots> slots_{};
  std::array<bool, kSnapshotRingSlots> filled_{};
  size_t next_ = 0;
};

}  // namespace net
