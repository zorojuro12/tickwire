#pragma once

#include <cstdint>

#include "net/snapshot_ring.h"

namespace client {

// Two full snapshot intervals at 60 Hz: kSnapshotIntervalTicks (3) * 2 = 6
// ticks = 100 ms. One interval would leave zero margin -- a single late or
// lost snapshot would starve the timeline on every occurrence. Two means a
// lost snapshot costs nothing visible at all.
inline constexpr uint32_t kInterpDelayTicks = 6;

// |error| at or beyond this: jump rather than crawl. 18 ticks is 300 ms,
// far past anything the 1-tick-per-snapshot nudge should be asked to close.
inline constexpr int32_t kInterpSnapErrorTicks = 18;

// The render timeline for remote entities, deliberately behind the newest
// snapshot, and the interpolation that reads it.
//
// Unlike ClockSync this needs no EMA and no cooldown. ClockSync closes a loop
// over real dead time -- it observes the delayed consequence of its own past
// corrections, which is why reacting to every observation there produced a
// GROWING oscillation. Here the observed signal is the snapshot's own tick,
// which this class's corrections do not influence at all, so a plain
// snap-or-nudge is both sufficient and correct.
class Interpolator {
 public:
  void advance() noexcept;                               // once per client tick
  void observe(uint32_t newest_snapshot_tick) noexcept;  // once per accepted snapshot
  uint32_t renderTick() const noexcept;
  bool haveTimeline() const noexcept;
  uint32_t snaps() const noexcept;

  // The interpolated position of player_id at renderTick(). False when the
  // ring holds nothing usable for that player. Never extrapolates.
  bool sample(const net::SnapshotRing& ring, uint32_t player_id, float& x, float& y) const noexcept;

 private:
  uint32_t render_tick_ = 0;
  bool have_ = false;
  uint32_t snaps_ = 0;
};

}  // namespace client
