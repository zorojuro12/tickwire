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

  const int64_t error = static_cast<int64_t>(target) - static_cast<int64_t>(render_tick_);
  if (error >= kInterpSnapErrorTicks || error <= -kInterpSnapErrorTicks) {
    render_tick_ = target;
    ++snaps_;
  } else if (error > 0) {
    ++render_tick_;
  } else if (error < 0) {
    if (render_tick_ > 0) --render_tick_;
  }
}

uint32_t Interpolator::renderTick() const noexcept { return render_tick_; }

bool Interpolator::haveTimeline() const noexcept { return have_; }

uint32_t Interpolator::snaps() const noexcept { return snaps_; }

bool Interpolator::sample(const net::SnapshotRing&, uint32_t, float&, float&) const noexcept {
  return false;
}

}  // namespace client
