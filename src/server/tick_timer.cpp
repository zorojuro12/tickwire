#include "server/tick_timer.h"

#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace server {

TickTimer::TickTimer(uint32_t hz) noexcept {
  if (hz == 0) return;

  const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (fd < 0) return;

  const int64_t period_ns = 1'000'000'000LL / static_cast<int64_t>(hz);
  struct itimerspec spec {};
  spec.it_value.tv_sec = period_ns / 1'000'000'000LL;
  spec.it_value.tv_nsec = period_ns % 1'000'000'000LL;
  spec.it_interval = spec.it_value;

  if (timerfd_settime(fd, 0, &spec, nullptr) != 0) {
    ::close(fd);
    return;
  }
  fd_ = fd;
}

TickTimer::~TickTimer() {
  if (fd_ >= 0) ::close(fd_);
}

TickTimer::TickTimer(TickTimer&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

TickTimer& TickTimer::operator=(TickTimer&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

bool TickTimer::valid() const noexcept { return fd_ >= 0; }
int TickTimer::fd() const noexcept { return fd_; }

uint64_t TickTimer::consumeExpirations() noexcept {
  if (fd_ < 0) return 0;
  uint64_t count = 0;
  for (;;) {
    const ssize_t n = ::read(fd_, &count, sizeof(count));
    if (n == static_cast<ssize_t>(sizeof(count))) return count;
    if (n < 0 && errno == EINTR) continue;
    return 0;
  }
}

}  // namespace server
