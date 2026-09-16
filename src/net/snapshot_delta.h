#pragma once

#include <array>
#include <cstdint>

#include "net/bytes.h"
#include "net/transport.h"
#include "sim/sim.h"

namespace net {

inline constexpr size_t kSnapshotDeltaFixedBytes = 16;   // tick + baseline_tick + 2 masks
inline constexpr size_t kDeltaRecordBytes = 20;          // x, y, vx, vy, radius (id is in the mask)
static_assert(kSnapshotDeltaFixedBytes + sim::kMaxPlayers * kDeltaRecordBytes <= kMaxPacket);

// present_mask/changed_mask are each a single uint32_t, one bit per id -- the
// wire layout has no room to address an id beyond bit 31. decodeSnapshotDelta
// derives record_count from a popcount over the full 32-bit changed_mask (as
// read off the wire) and fills net::SnapshotDelta::records (sized
// sim::kMaxPlayers) by walking id 1..sim::kMaxPlayers; those two only agree
// because kMaxPlayers happens to equal 32 today. If kMaxPlayers is ever
// changed, that agreement breaks silently and decodeSnapshotDelta's
// finiteness-validation loop reads records[i] past the end of the array for
// an attacker-controlled high mask bit -- turn that into a compile error
// instead of a latent out-of-bounds read.
static_assert(sim::kMaxPlayers == 32,
              "present_mask/changed_mask are uint32_t bitmasks, one bit per id -- "
              "changing kMaxPlayers away from 32 requires re-deriving decodeSnapshotDelta's "
              "record_count bound, not just this constant");

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
