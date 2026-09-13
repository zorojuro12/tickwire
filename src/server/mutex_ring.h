#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace server {

// The benchmark's mutex arm: same four-call contract as PacketRing, safe to
// call from two real threads. One mutex guards the whole ring; every call
// takes it for its entire body.
template <typename T, size_t N>
class MutexRing {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");

 public:
  T* acquireWrite() noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    if (write_ - read_ == N) return nullptr;
    return &buf_[write_ & (N - 1)];
  }
  void commitWrite() noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    ++write_;
  }

  T* acquireRead() noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    if (write_ == read_) return nullptr;
    return &buf_[read_ & (N - 1)];
  }
  void commitRead() noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    ++read_;
  }

  size_t size() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<size_t>(write_ - read_);
  }
  static constexpr size_t capacity() noexcept { return N; }

 private:
  std::array<T, N> buf_{};
  uint64_t write_ = 0;
  uint64_t read_ = 0;
  mutable std::mutex mu_;
};

}  // namespace server
