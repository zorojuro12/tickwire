#include "server/input_buffer.h"

namespace server {

bool InputBuffer::push(const sim::InputCommand& in) noexcept {
  const uint32_t i = in.tick & kInputBufferMask;
  slots_[i] = in;
  filled_[i] = true;
  return true;
}

bool InputBuffer::takeFor(uint32_t tick, sim::InputCommand& out) noexcept {
  const uint32_t i = tick & kInputBufferMask;
  const bool hit = filled_[i] && slots_[i].tick == tick;
  if (hit) out = slots_[i];
  filled_[i] = false;
  return hit;
}

}  // namespace server
