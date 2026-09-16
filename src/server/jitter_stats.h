#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace server {

// Fixed-capacity sample recorder for tick-interval / handoff-latency
// measurement. No allocation, so it is safe on a hot path recording once per
// tick or once per queue handoff. Percentiles are nearest-rank, computed by
// sorting in place -- intended to run once after a measurement run ends, not
// on the hot path itself.
template <size_t N>
class JitterStats {
 public:
  void record(uint64_t interval_ns) noexcept {
    if (count_ >= N) {
      ++dropped_;
      return;
    }
    samples_[count_++] = interval_ns;
    sorted_ = false;
  }

  size_t count() const noexcept { return count_; }
  size_t dropped() const noexcept { return dropped_; }

  // Nearest rank, no interpolation: idx = clamp(ceil(p * n) - 1, 0, n - 1).
  uint64_t percentileNs(double p) noexcept {
    if (count_ == 0) return 0;
    sortIfNeeded();
    const double raw = std::ceil(p * static_cast<double>(count_)) - 1.0;
    const size_t idx = static_cast<size_t>(
        std::clamp(raw, 0.0, static_cast<double>(count_ - 1)));
    return samples_[idx];
  }

  uint64_t maxNs() noexcept {
    if (count_ == 0) return 0;
    sortIfNeeded();
    return samples_[count_ - 1];
  }

 private:
  void sortIfNeeded() noexcept {
    if (sorted_) return;
    std::sort(samples_.begin(), samples_.begin() + static_cast<ptrdiff_t>(count_));
    sorted_ = true;
  }

  std::array<uint64_t, N> samples_{};
  size_t count_ = 0;
  size_t dropped_ = 0;
  bool sorted_ = false;
};

}  // namespace server
