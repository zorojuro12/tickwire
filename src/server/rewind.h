#pragma once

#include <cstdint>

#include "net/snapshot_ring.h"
#include "server/session.h"
#include "sim/world.h"

namespace server {

// Derivation: when pass 2 of tick F runs, history_ (net::kSnapshotRingSlots =
// 16 snapshots, kSnapshotIntervalTicks = 3 apart) holds a newest entry at a
// tick >= F - kSnapshotIntervalTicks. Its oldest entry is therefore at a tick
// >= F - (kSnapshotRingSlots - 1) * kSnapshotIntervalTicks = F - 48, and at
// most F - 46. A view tick no more than 45 behind F is always at or after
// the oldest entry, so the bracketing snapshot the client sampled is
// guaranteed still to be in the server's ring. Beyond that, the server could
// silently sample a *different* bracket than the client did, so it refuses
// instead. 45 ticks is 750 ms.
inline constexpr uint32_t kMaxRewindTicks = 45;

struct RewindRequest {
  uint32_t shooter;
  uint32_t view_tick;
  uint32_t fire_tick;
};

// Builds the world as `req.shooter` drew it: the shooter at its live
// position, every other live player sampled from `history` at
// `req.view_tick` via the same net::samplePlayerAt the client rendered with.
// False, leaving `out` untouched, when the shooter is absent from `live` or
// the request is implausible (see rewind.cpp). Never writes to `live`,
// `history`, or `sessions` -- purely a read.
bool buildRewoundView(const sim::World& live, const net::SnapshotRing& history,
                      const SessionTable& sessions, const RewindRequest& req,
                      sim::WorldSnapshot& out) noexcept;

}  // namespace server
