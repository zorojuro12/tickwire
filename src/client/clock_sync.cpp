#include "client/clock_sync.h"

namespace client {

void ClockSync::observe(uint32_t server_tick, uint32_t ack_tick) noexcept {
  if (ack_tick == 0) return;

  const int64_t lead = static_cast<int64_t>(ack_tick) - static_cast<int64_t>(server_tick);
  const int64_t error = lead - kTargetLeadTicks;
  lead_ = static_cast<int32_t>(lead);
  have_ = true;

  if (error >= kSnapErrorTicks || error <= -kSnapErrorTicks) {
    int64_t correction = -error;
    if (correction > kMaxCorrectionTicks) correction = kMaxCorrectionTicks;
    if (correction < -kMaxCorrectionTicks) correction = -kMaxCorrectionTicks;
    pending_ = static_cast<int32_t>(correction);
    ++snaps_;
  } else if (error == 0) {
    pending_ = 0;
  } else if (error > 0) {
    pending_ = -1;
  } else {
    pending_ = 1;
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
