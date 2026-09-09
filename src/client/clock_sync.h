#pragma once

#include <cstdint>

namespace client {

inline constexpr int32_t kTargetLeadTicks = 3;      // input ticks the server should hold
inline constexpr int32_t kSnapErrorTicks = 12;      // |error| at or beyond this: correct at once
inline constexpr int32_t kMaxCorrectionTicks = 30;  // hard clamp on a single correction
inline constexpr float kSmoothingAlpha = 0.2f;      // EMA weight per observation
inline constexpr float kDeadbandTicks = 1.0f;       // |smoothed error| at or below this: no nudge
inline constexpr uint32_t kCorrectionCooldownObservations = 5;  // min gap between nudges

// A pure controller: turns the server-reported input-buffer depth
// (ack_tick - tick, on a snapshot) into a per-tick clock correction. Never
// touches a clock, a packet, or a transport.
//
// The nudge decision is driven by an EMA of the observed lead, not the raw
// per-snapshot value, AND is rate-limited to at most one nudge every
// kCorrectionCooldownObservations. This loop closes over substantial dead
// time: by the time this client sees a snapshot, it already reflects the
// server's state from roughly one round trip ago. Reacting to every
// observation -- even a smoothed one -- lets several corrections stack up
// "in flight" (already sent, not yet reflected in the next observation)
// before their combined effect is even visible, which measurably produces
// a GROWING, not decaying, oscillation under real latency (verified via
// P3 Task 8's convergence test: EMA alone slowed the period but the
// amplitude kept climbing). The cooldown is what actually damps it, by
// giving each correction time to be reflected in a subsequent observation
// before another one is allowed to fire. The first-ever observation seeds
// the EMA at its own raw value rather than blending against nothing, and
// the snap path (for genuinely large errors) is exempt from the cooldown,
// so bootstrapping and recovery from a large desync stay immediate.
class ClockSync {
 public:
  // Feed one accepted snapshot.
  void observe(uint32_t server_tick, uint32_t ack_tick) noexcept;

  // The correction to apply to the client's tick this frame, then cleared.
  int32_t takeCorrection() noexcept;

  bool haveEstimate() const noexcept;  // has any snapshot been observed?
  int32_t lead() const noexcept;       // last observed (raw) ack_tick - server_tick
  uint32_t snaps() const noexcept;     // corrections that crossed kSnapErrorTicks

 private:
  bool have_ = false;
  int32_t lead_ = 0;          // last raw observation, for lead()
  float smoothed_lead_ = 0.0f;
  int32_t pending_ = 0;
  uint32_t snaps_ = 0;
  uint32_t observations_since_correction_ = 0;
};

}  // namespace client
