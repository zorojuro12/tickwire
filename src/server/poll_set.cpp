#include "server/poll_set.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>

namespace server {

PollSet::PollSet() noexcept : fd_(epoll_create1(EPOLL_CLOEXEC)) {}

PollSet::~PollSet() {
  if (fd_ >= 0) ::close(fd_);
}

PollSet::PollSet(PollSet&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

PollSet& PollSet::operator=(PollSet&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

bool PollSet::valid() const noexcept { return fd_ >= 0; }

bool PollSet::add(int fd, uint32_t token) noexcept {
  if (fd_ < 0 || fd < 0) return false;
  struct epoll_event ev {};
  ev.events = EPOLLIN;
  ev.data.u32 = token;
  return epoll_ctl(fd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}

size_t PollSet::wait(int timeout_ms, std::span<uint32_t> out) noexcept {
  if (fd_ < 0) return 0;
  std::array<struct epoll_event, 8> events{};
  int n = epoll_wait(fd_, events.data(), static_cast<int>(events.size()), timeout_ms);
  if (n < 0 && errno == EINTR) {
    n = epoll_wait(fd_, events.data(), static_cast<int>(events.size()), timeout_ms);
  }
  if (n <= 0) return 0;
  const size_t count = std::min(static_cast<size_t>(n), out.size());
  for (size_t i = 0; i < count; ++i) out[i] = events[i].data.u32;
  return count;
}

}  // namespace server
