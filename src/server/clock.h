#pragma once

#include <cstdint>

namespace server {

// CLOCK_MONOTONIC, wrapped to 32 bits. Wraps every ~49 days, which is fine
// because every consumer uses differences. This is the only clock_gettime
// call in the project; only apps/ calls it.
uint32_t monotonicMs() noexcept;

}  // namespace server
