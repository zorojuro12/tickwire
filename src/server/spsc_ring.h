#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace server {

// The lock-free arm, exactly per architecture-resolution doc § Q4. Capacity
// is a power of two; indices are monotonic uint64_t, never wrapped (masking
// happens only at access). Producer loads read_ acquire / stores write_
// release; consumer loads write_ acquire / stores read_ release -- no
// seq_cst on the hot path. alignas(64) on both indices and the buffer keeps
// the two indices off the same cache line, so a benchmark against this ring
// measures synchronization cost, not false sharing.
template <typename T, size_t N>
class SpscRing {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");

 public:
  T* acquireWrite() noexcept {
    const uint64_t w = write_.load(std::memory_order_relaxed);
    const uint64_t r = read_.load(std::memory_order_acquire);
    if (w - r == N) return nullptr;
    return &buf_[w & (N - 1)];
  }
  void commitWrite() noexcept {
    const uint64_t w = write_.load(std::memory_order_relaxed);
    write_.store(w + 1, std::memory_order_release);
  }

  T* acquireRead() noexcept {
    const uint64_t r = read_.load(std::memory_order_relaxed);
    const uint64_t w = write_.load(std::memory_order_acquire);
    if (w == r) return nullptr;
    return &buf_[r & (N - 1)];
  }
  void commitRead() noexcept {
    const uint64_t r = read_.load(std::memory_order_relaxed);
    read_.store(r + 1, std::memory_order_release);
  }

  size_t size() const noexcept {
    return static_cast<size_t>(write_.load(std::memory_order_acquire) -
                                read_.load(std::memory_order_acquire));
  }
  static constexpr size_t capacity() noexcept { return N; }

  // Diagnostic only, never called on the hot path: proves the alignas(64)
  // separation between the two indices is real, not just aspirational.
  size_t indexByteSeparation() const noexcept {
    const auto* w = reinterpret_cast<const std::byte*>(&write_);
    const auto* r = reinterpret_cast<const std::byte*>(&read_);
    return w > r ? static_cast<size_t>(w - r) : static_cast<size_t>(r - w);
  }

 private:
  alignas(64) std::atomic<uint64_t> write_{0};
  alignas(64) std::atomic<uint64_t> read_{0};
  alignas(64) std::array<T, N> buf_{};
};

}  // namespace server
