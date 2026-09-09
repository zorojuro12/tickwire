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

// The magnitude of each reconciliation correction, over a rolling window.
// Fixed storage, no allocation; percentiles sort a stack copy on query,
// which is a diagnostic path, never a per-tick one.
//
// Percentile convention: nearest-rank over the window's live entries --
// sort the n live samples ascending and take index ceil(p/100 * n) - 1.
class PredictionStats {
 public:
  static constexpr size_t kWindow = 512;  // ~25 s of snapshots at 20 Hz

  void record(float error) noexcept;
  uint32_t samples() const noexcept;  // total recorded, not window-capped
  float p50() const noexcept;         // 0 when samples() == 0
  float p99() const noexcept;         // 0 when samples() == 0
  float worst() const noexcept;       // over all samples, not just the window

 private:
  float percentile(float p) const noexcept;

  std::array<float, kWindow> window_{};
  uint32_t count_ = 0;
  float worst_ = 0.0f;
};

}  // namespace client
