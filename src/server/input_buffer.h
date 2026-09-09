#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "sim/sim.h"

namespace server {

inline constexpr size_t kInputBufferSlots = 16;  // power of two; 266 ms of input at 60 Hz
inline constexpr uint32_t kInputBufferMask = kInputBufferSlots - 1;

// Per-session, tick-keyed input storage. Pure: no I/O, no clock, no
// allocation. Indexed by tick & kInputBufferMask, so lookup is O(1) and the
// acceptance window is what prevents two ticks aliasing onto one slot.
class InputBuffer {
 public:
  // Accepts `in` only when its tick falls in the open-closed acceptance
  // window (lastConsumed, lastConsumed + kInputBufferSlots]. Rejects a tick
  // of 0, a tick already simulated past, and a tick so far ahead it would
  // alias onto a slot the window still owns.
  bool push(const sim::InputCommand& in) noexcept;

  // Writes the input stamped exactly `tick` into `out` and returns true.
  // Returns false on an underrun -- no input is held for that tick. Either
  // way the consumption floor advances to `tick` and the slot is cleared,
  // so a client that goes silent does not freeze the acceptance window.
  bool takeFor(uint32_t tick, sim::InputCommand& out) noexcept;

  uint32_t highestTick() const noexcept;  // highest accepted tick; 0 when none
  uint32_t lastConsumed() const noexcept; // the consumption floor; 0 initially
  void reset() noexcept;                  // clears every slot and both counters

 private:
  std::array<sim::InputCommand, kInputBufferSlots> slots_{};
  std::array<bool, kInputBufferSlots> filled_{};
  uint32_t highest_tick_ = 0;
  uint32_t last_consumed_ = 0;
};

}  // namespace server
