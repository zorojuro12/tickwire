#include "server/rewind.h"

namespace server {

bool buildRewoundView(const sim::World& live, const net::SnapshotRing& history,
                      const SessionTable& /*sessions*/, const RewindRequest& req,
                      sim::WorldSnapshot& out) noexcept {
  sim::WorldSnapshot live_snap{};
  live.writeSnapshot(live_snap);

  const sim::PlayerState* shooter_rec = nullptr;
  for (uint32_t i = 0; i < live_snap.count; ++i) {
    if (live_snap.players[i].id == req.shooter) {
      shooter_rec = &live_snap.players[i];
      break;
    }
  }
  if (shooter_rec == nullptr) return false;

  sim::WorldSnapshot result{};
  result.tick = req.view_tick;
  uint32_t written = 0;
  result.players[written++] = *shooter_rec;

  for (uint32_t i = 0; i < live_snap.count; ++i) {
    const sim::PlayerState& p = live_snap.players[i];
    if (p.id == req.shooter) continue;
    float x = 0.0f, y = 0.0f;
    if (!net::samplePlayerAt(history, p.id, req.view_tick, x, y)) continue;
    sim::PlayerState rewound = p;
    rewound.x = x;
    rewound.y = y;
    result.players[written++] = rewound;
  }
  result.count = written;

  out = result;
  return true;
}

}  // namespace server
