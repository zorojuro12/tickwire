#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace server {

// The I/O<->simulation seam. Single-threaded, holds no atomics, and pairs
// with no memory ordering -- this is not SpscRing. P5 introduces the
// lock-free version behind the same four calls and benchmarks one against
// the other.
template <typename T, size_t N>
class PacketRing {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");

 public:
  T* acquireWrite() noexcept {
    if (write_ - read_ == N) return nullptr;
    return &buf_[write_ & (N - 1)];
  }
  void commitWrite() noexcept { ++write_; }

  T* acquireRead() noexcept {
    if (write_ == read_) return nullptr;
    return &buf_[read_ & (N - 1)];
  }
  void commitRead() noexcept { ++read_; }

  size_t size() const noexcept { return static_cast<size_t>(write_ - read_); }
  static constexpr size_t capacity() noexcept { return N; }

 private:
  std::array<T, N> buf_{};
  uint64_t write_ = 0;
  uint64_t read_ = 0;
};

}  // namespace server
