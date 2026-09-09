#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "sim/sim.h"

namespace client {

inline constexpr size_t kPendingInputSlots = 128;  // ~2.1 s of input at 60 Hz
inline constexpr uint32_t kPendingInputMask = kPendingInputSlots - 1;

struct PendingInput {
  sim::InputCommand cmd;
  uint32_t send_time_ms;
};

// Every input sent and not yet known to be consumed, indexed by tick so a
// replay can ask for one tick at a time. Older than kPendingInputSlots ticks
// is silently evicted -- a client that far behind has already snapped.
class PendingInputs {
 public:
  void record(const sim::InputCommand& in, uint32_t send_time_ms) noexcept;
  const PendingInput* find(uint32_t tick) const noexcept;  // nullptr when absent

 private:
  std::array<PendingInput, kPendingInputSlots> slots_{};
  std::array<bool, kPendingInputSlots> filled_{};
};

}  // namespace client
