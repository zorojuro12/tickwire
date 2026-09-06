#include "net/udp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace net {

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

  for (;;) {
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const ssize_t n = ::recvfrom(fd_, slot.data.data(), kMaxPacket, MSG_TRUNC,
                                  reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (static_cast<size_t>(n) > kMaxPacket) continue;  // truncated: discard and drain onward

    slot.len = static_cast<uint16_t>(n);
    slot.peer.addr_be = from.sin_addr.s_addr;
    slot.peer.port_be = from.sin_port;
    return true;
  }
}

}  // namespace net
