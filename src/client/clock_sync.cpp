#include "client/clock_sync.h"

namespace client {

void ClockSync::observe(uint32_t server_tick, uint32_t ack_tick) noexcept {
  if (ack_tick == 0) return;

  const int64_t raw_lead = static_cast<int64_t>(ack_tick) - static_cast<int64_t>(server_tick);
  lead_ = static_cast<int32_t>(raw_lead);

  if (!have_) {
    smoothed_lead_ = static_cast<float>(raw_lead);
    have_ = true;
  } else {
    smoothed_lead_ += kSmoothingAlpha * (static_cast<float>(raw_lead) - smoothed_lead_);
  }

  const float error = smoothed_lead_ - static_cast<float>(kTargetLeadTicks);
  ++observations_since_correction_;

  if (error >= static_cast<float>(kSnapErrorTicks) || error <= -static_cast<float>(kSnapErrorTicks)) {
    int64_t correction = -static_cast<int64_t>(error);
    if (correction > kMaxCorrectionTicks) correction = kMaxCorrectionTicks;
    if (correction < -kMaxCorrectionTicks) correction = -kMaxCorrectionTicks;
    pending_ = static_cast<int32_t>(correction);
    ++snaps_;
    observations_since_correction_ = 0;
    // A snap jumps the clock directly to the target lead; reset the
    // average to match so the next observation isn't judged against a
    // now-stale pre-snap history.
    smoothed_lead_ = static_cast<float>(kTargetLeadTicks);
  } else if (observations_since_correction_ < kCorrectionCooldownObservations) {
    // A prior nudge hasn't had time to be reflected in a new observation
    // yet -- do nothing rather than stack another one on top of it.
    pending_ = 0;
  } else if (error > kDeadbandTicks) {
    pending_ = -1;
    observations_since_correction_ = 0;
  } else if (error < -kDeadbandTicks) {
    pending_ = 1;
    observations_since_correction_ = 0;
  } else {
    pending_ = 0;
  }
}

int32_t ClockSync::takeCorrection() noexcept {
  const int32_t c = pending_;
  pending_ = 0;
  return c;
}

bool ClockSync::haveEstimate() const noexcept { return have_; }

int32_t ClockSync::lead() const noexcept { return lead_; }

uint32_t ClockSync::snaps() const noexcept { return snaps_; }

}  // namespace client
