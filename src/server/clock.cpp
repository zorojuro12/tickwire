#include "server/clock.h"

#include <ctime>

namespace server {

uint32_t monotonicMs() noexcept {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t ms =
      static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
  return static_cast<uint32_t>(ms);
}

uint64_t monotonicNs() noexcept {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

}  // namespace server
