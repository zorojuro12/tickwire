#pragma once

#include <array>
#include <cstdint>

#include "net/bytes.h"
#include "sim/sim.h"

namespace net {

inline constexpr size_t kSnapshotDeltaFixedBytes = 16;   // tick + baseline_tick + 2 masks
inline constexpr size_t kDeltaRecordBytes = 20;          // x, y, vx, vy, radius (id is in the mask)

// One decoded kSnapshotDelta payload. Records are ascending by player id and
// correspond, in order, to the set bits of changed_mask.
struct SnapshotDelta {
  uint32_t tick;
  uint32_t baseline_tick;
  uint32_t present_mask;   // bit (id - 1) set: player id is in this snapshot
  uint32_t changed_mask;   // bit (id - 1) set: a record for id follows; else copy the baseline
  uint32_t record_count;   // == popcount(changed_mask)
  std::array<sim::PlayerState, sim::kMaxPlayers> records;  // .id filled in from the mask
};

// Encodes `current` as a delta against `baseline`. False if the writer runs out
// of room or either snapshot holds an out-of-range id or count.
bool encodeSnapshotDelta(const sim::WorldSnapshot& baseline,
                          const sim::WorldSnapshot& current, ByteWriter& w);
bool decodeSnapshotDelta(ByteReader& r, SnapshotDelta& out);

// Reconstructs the full snapshot. False when the delta names a player that is
// present-but-unchanged and absent from the baseline (unreconstructable), or
// when baseline.tick != d.baseline_tick.
bool applySnapshotDelta(const sim::WorldSnapshot& baseline, const SnapshotDelta& d,
                         sim::WorldSnapshot& out);

}  // namespace net
