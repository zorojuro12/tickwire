#include "client/interpolation.h"

#include "sim/sim.h"

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

namespace {

const sim::PlayerState* findById(const sim::WorldSnapshot& s, uint32_t id) {
  for (uint32_t i = 0; i < s.count; ++i) {
    if (s.players[i].id == id) return &s.players[i];
  }
  return nullptr;
}

}  // namespace

bool Interpolator::sample(const net::SnapshotRing& ring, uint32_t player_id, float& x,
                           float& y) const noexcept {
  if (!have_) return false;

  const sim::WorldSnapshot* a = ring.newestAtOrBefore(render_tick_);
  const sim::WorldSnapshot* b = ring.oldestAfter(render_tick_);
  if (a == nullptr || b == nullptr) return false;

  const sim::PlayerState* pa = findById(*a, player_id);
  const sim::PlayerState* pb = findById(*b, player_id);
  if (pa == nullptr || pb == nullptr) return false;

  const float alpha = static_cast<float>(render_tick_ - a->tick) /
                       static_cast<float>(b->tick - a->tick);
  x = pa->x + (pb->x - pa->x) * alpha;
  y = pa->y + (pb->y - pa->y) * alpha;
  return true;
}

}  // namespace client
