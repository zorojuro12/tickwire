#include "client/interpolation.h"

namespace client {

void Interpolator::advance() noexcept {
  if (have_) ++render_tick_;
}

void Interpolator::observe(uint32_t newest_snapshot_tick) noexcept {
  const uint32_t target =
      newest_snapshot_tick >= kInterpDelayTicks ? newest_snapshot_tick - kInterpDelayTicks : 0;
  if (!have_) {
    render_tick_ = target;
    have_ = true;
    return;
  }
}

uint32_t Interpolator::renderTick() const noexcept { return render_tick_; }

bool Interpolator::haveTimeline() const noexcept { return have_; }

uint32_t Interpolator::snaps() const noexcept { return snaps_; }

bool Interpolator::sample(const net::SnapshotRing&, uint32_t, float&, float&) const noexcept {
  return false;
}

}  // namespace client
