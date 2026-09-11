#pragma once

#include <cstdint>

namespace server {

// RAII timerfd, periodic at `hz`.
class TickTimer {
 public:
  explicit TickTimer(uint32_t hz) noexcept;
  ~TickTimer();

  TickTimer(const TickTimer&) = delete;
  TickTimer& operator=(const TickTimer&) = delete;
  TickTimer(TickTimer&& other) noexcept;
  TickTimer& operator=(TickTimer&& other) noexcept;

  bool valid() const noexcept;
  int fd() const noexcept;
  uint64_t consumeExpirations() noexcept;  // 0 when nothing has fired

 private:
  int fd_ = -1;
};

}  // namespace server
