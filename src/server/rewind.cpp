#include "server/rewind.h"

namespace server {

bool buildRewoundView(const sim::World& live, const net::SnapshotRing& history,
                      const SessionTable& sessions, const RewindRequest& req,
                      sim::WorldSnapshot& out) noexcept {
  // view_tick == 0 means "uncompensated" -- never a real rewind request.
  if (req.view_tick == 0) return false;
  // A view tick not strictly before the fire tick claims to have drawn a
  // world that did not exist yet when the shot was fired.
  if (req.view_tick >= req.fire_tick) return false;
  // Cannot underflow: the check above already guarantees fire_tick > view_tick.
  if (req.fire_tick - req.view_tick > kMaxRewindTicks) return false;
  // The shooter claims to have drawn a snapshot it never acknowledged
  // holding -- ackedSnapshotTick(shooter) is 0 for an unknown player, which
  // view_tick > 0 already exceeds.
  if (req.view_tick > sessions.ackedSnapshotTick(req.shooter)) return false;

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
