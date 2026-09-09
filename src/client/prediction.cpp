#include "client/prediction.h"

#include <algorithm>
#include <cmath>

namespace client {

void PendingInputs::record(const sim::InputCommand& in, uint32_t send_time_ms) noexcept {
  const uint32_t i = in.tick & kPendingInputMask;
  slots_[i] = PendingInput{in, send_time_ms};
  filled_[i] = true;
}

const PendingInput* PendingInputs::find(uint32_t tick) const noexcept {
  const uint32_t i = tick & kPendingInputMask;
  if (!filled_[i] || slots_[i].cmd.tick != tick) return nullptr;
  return &slots_[i];
}

void PredictionStats::record(float error) noexcept {
  if (!std::isfinite(error)) return;
  window_[count_ % kWindow] = error;
  ++count_;
  if (error > worst_) worst_ = error;
}

uint32_t PredictionStats::samples() const noexcept { return count_; }

float PredictionStats::worst() const noexcept { return worst_; }

float PredictionStats::percentile(float p) const noexcept {
  const uint32_t n = count_ < kWindow ? count_ : static_cast<uint32_t>(kWindow);
  if (n == 0) return 0.0f;

  std::array<float, kWindow> sorted{};
  for (uint32_t i = 0; i < n; ++i) sorted[i] = window_[i];
  std::sort(sorted.begin(), sorted.begin() + n);

  size_t index = static_cast<size_t>(std::ceil(p / 100.0f * static_cast<float>(n))) - 1;
  if (index >= n) index = n - 1;
  return sorted[index];
}

float PredictionStats::p50() const noexcept { return percentile(50.0f); }

float PredictionStats::p99() const noexcept { return percentile(99.0f); }

}  // namespace client
