#pragma once

#include <cstdint>

namespace client {

inline constexpr int32_t kTargetLeadTicks = 3;      // input ticks the server should hold
inline constexpr int32_t kSnapErrorTicks = 12;      // |error| at or beyond this: correct at once
inline constexpr int32_t kMaxCorrectionTicks = 30;  // hard clamp on a single correction

// A pure controller: turns the server-reported input-buffer depth
// (ack_tick - tick, on a snapshot) into a per-tick clock correction. Never
// touches a clock, a packet, or a transport.
class ClockSync {
 public:
  // Feed one accepted snapshot.
  void observe(uint32_t server_tick, uint32_t ack_tick) noexcept;

  // The correction to apply to the client's tick this frame, then cleared.
  int32_t takeCorrection() noexcept;

  bool haveEstimate() const noexcept;  // has any snapshot been observed?
  int32_t lead() const noexcept;       // last observed ack_tick - server_tick

 private:
  bool have_ = false;
  int32_t lead_ = 0;
  int32_t pending_ = 0;
};

}  // namespace client
