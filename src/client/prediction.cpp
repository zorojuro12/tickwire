#include "client/prediction.h"

namespace client {

void PendingInputs::record(const sim::InputCommand& in, uint32_t send_time_ms) noexcept {
  const uint32_t i = in.tick & kPendingInputMask;
  slots_[i] = PendingInput{in, send_time_ms};
  filled_[i] = true;
}

const PendingInput* PendingInputs::find(uint32_t tick) const noexcept {
  const uint32_t i = tick & kPendingInputMask;
  if (!filled_[i] || slots_[i].cmd.tick != tick) return nullptr;
  return &slots_[i];
}

}  // namespace client
