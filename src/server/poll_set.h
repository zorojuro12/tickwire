#pragma once

#include <cstdint>
#include <span>

namespace server {

// RAII epoll fd multiplexing readiness across registered fds.
class PollSet {
 public:
  PollSet() noexcept;
  ~PollSet();

  PollSet(const PollSet&) = delete;
  PollSet& operator=(const PollSet&) = delete;
  PollSet(PollSet&& other) noexcept;
  PollSet& operator=(PollSet&& other) noexcept;

  bool valid() const noexcept;
  bool add(int fd, uint32_t token) noexcept;  // level-triggered read readiness
  // Waits up to timeout_ms; writes ready tokens into `out`, returns how many.
  size_t wait(int timeout_ms, std::span<uint32_t> out) noexcept;

 private:
  int fd_ = -1;
};

}  // namespace server
