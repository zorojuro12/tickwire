#include "server/input_buffer.h"

namespace server {

bool InputBuffer::push(const sim::InputCommand& in) noexcept {
  if (in.tick <= last_consumed_ || in.tick > last_consumed_ + kInputBufferSlots) {
    return false;
  }
  const uint32_t i = in.tick & kInputBufferMask;
  slots_[i] = in;
  filled_[i] = true;
  if (in.tick > highest_tick_) highest_tick_ = in.tick;
  return true;
}

bool InputBuffer::takeFor(uint32_t tick, sim::InputCommand& out) noexcept {
  const uint32_t i = tick & kInputBufferMask;
  const bool hit = filled_[i] && slots_[i].tick == tick;
  if (hit) out = slots_[i];
  filled_[i] = false;
  last_consumed_ = tick;
  return hit;
}

uint32_t InputBuffer::highestTick() const noexcept { return highest_tick_; }

uint32_t InputBuffer::lastConsumed() const noexcept { return last_consumed_; }

void InputBuffer::reset() noexcept {
  filled_.fill(false);
  highest_tick_ = 0;
  last_consumed_ = 0;
}

}  // namespace server
