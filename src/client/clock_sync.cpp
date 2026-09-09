#include "client/clock_sync.h"

namespace client {

void ClockSync::observe(uint32_t server_tick, uint32_t ack_tick) noexcept {
  const int64_t lead = static_cast<int64_t>(ack_tick) - static_cast<int64_t>(server_tick);
  const int64_t error = lead - kTargetLeadTicks;
  lead_ = static_cast<int32_t>(lead);
  have_ = true;
  if (error == 0) {
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

}  // namespace client
