#pragma once

#include <cstdint>

namespace server {

// CLOCK_MONOTONIC, wrapped to 32 bits. Wraps every ~49 days, which is fine
// because every consumer uses differences. This is the only clock_gettime
// call in the project; only apps/ calls it.
uint32_t monotonicMs() noexcept;

// CLOCK_MONOTONIC in nanoseconds, full 64-bit range. Millisecond resolution
// cannot resolve tick jitter against a 16.67ms budget; this can.
uint64_t monotonicNs() noexcept;

}  // namespace server
