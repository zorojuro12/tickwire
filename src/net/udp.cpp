#include "net/udp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace net {

UdpTransport::~UdpTransport() {
  if (fd_ >= 0) ::close(fd_);
}

UdpTransport::UdpTransport(UdpTransport&& other) noexcept
    : fd_(other.fd_), local_(other.local_) {
  other.fd_ = -1;
}

UdpTransport& UdpTransport::operator=(UdpTransport&& other) noexcept {
  if (this == &other) return *this;
  if (fd_ >= 0) ::close(fd_);
  fd_ = other.fd_;
  local_ = other.local_;
  other.fd_ = -1;
  return *this;
}

int UdpTransport::nativeHandle() const noexcept { return fd_; }

bool UdpTransport::bind(uint32_t addr_be, uint16_t port_be) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  if (fd < 0) return false;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = addr_be;
  addr.sin_port = port_be;

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return false;
  }

  sockaddr_in bound{};
  socklen_t bound_len = sizeof(bound);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
    ::close(fd);
    return false;
  }

  if (fd_ >= 0) ::close(fd_);
  fd_ = fd;
  local_.addr_be = bound.sin_addr.s_addr;
  local_.port_be = bound.sin_port;
  return true;
}

Endpoint UdpTransport::localEndpoint() const noexcept { return local_; }

bool UdpTransport::send(const Endpoint& to, std::span<const std::byte> payload) {
  if (fd_ < 0) return false;
  if (payload.size() > kMaxPacket) return false;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = to.addr_be;
  addr.sin_port = to.port_be;

  const ssize_t sent = ::sendto(fd_, payload.data(), payload.size(), 0,
                                 reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  return sent == static_cast<ssize_t>(payload.size());
}

bool UdpTransport::tryReceive(PacketSlot& slot) {
  if (fd_ < 0) return false;

  size_t skipped_this_call = 0;
  for (;;) {
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const ssize_t n = ::recvfrom(fd_, slot.data.data(), kMaxPacket, MSG_TRUNC,
                                  reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n < 0) {
      if (errno == EINTR) continue;  // not attacker-driven: uncapped retry
      return false;
    }
    if (static_cast<size_t>(n) > kMaxPacket) {
      ++oversized_skipped_;
      if (++skipped_this_call >= kMaxOversizedSkipsPerCall) return false;
      continue;  // truncated: discard and drain onward
    }

    slot.len = static_cast<uint16_t>(n);
    slot.peer.addr_be = from.sin_addr.s_addr;
    slot.peer.port_be = from.sin_port;
    return true;
  }
}

size_t UdpTransport::receiveBatch(std::span<PacketSlot> slots) {
  if (fd_ < 0) return 0;

  const size_t kBatchCap = 64;
  const size_t requested = std::min(slots.size(), kBatchCap);
  if (requested == 0) return 0;

  std::array<mmsghdr, kBatchCap> msgs{};
  std::array<iovec, kBatchCap> iovs{};
  std::array<sockaddr_in, kBatchCap> froms{};

  for (size_t i = 0; i < requested; ++i) {
    iovs[i].iov_base = slots[i].data.data();
    iovs[i].iov_len = kMaxPacket;
    std::memset(&froms[i], 0, sizeof(froms[i]));
    msgs[i].msg_hdr.msg_name = &froms[i];
    msgs[i].msg_hdr.msg_namelen = sizeof(froms[i]);
    msgs[i].msg_hdr.msg_iov = &iovs[i];
    msgs[i].msg_hdr.msg_iovlen = 1;
    msgs[i].msg_hdr.msg_control = nullptr;
    msgs[i].msg_hdr.msg_controllen = 0;
    msgs[i].msg_hdr.msg_flags = 0;
  }

  // MSG_TRUNC: report a truncated datagram's true length (not just iov_len),
  // matching tryReceive's oversized-detection semantics exactly.
  const int received = ::recvmmsg(fd_, msgs.data(), static_cast<unsigned int>(requested),
                                   MSG_DONTWAIT | MSG_TRUNC, nullptr);
  if (received <= 0) return 0;

  size_t filled = 0;
  for (int i = 0; i < received; ++i) {
    if (msgs[i].msg_len > kMaxPacket) {
      ++oversized_skipped_;
      continue;
    }
    // recvmmsg wrote message i's payload into slots[i].data (that's what
    // iovs[i] pointed at) -- once an earlier message in this batch has been
    // skipped, filled < i, and the bytes must move to slots[filled] along
    // with the metadata, or a later slot's reported length/sender ends up
    // paired with an earlier slot's actual payload.
    if (filled != static_cast<size_t>(i)) {
      std::memcpy(slots[filled].data.data(), slots[i].data.data(), msgs[i].msg_len);
    }
    slots[filled].len = static_cast<uint16_t>(msgs[i].msg_len);
    slots[filled].peer.addr_be = froms[i].sin_addr.s_addr;
    slots[filled].peer.port_be = froms[i].sin_port;
    ++filled;
  }
  return filled;
}

}  // namespace net
