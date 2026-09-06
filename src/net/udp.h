#pragma once

#include <cstdint>
#include <span>

#include "net/transport.h"

namespace net {

// Non-blocking IPv4 UDP transport over a raw POSIX socket.
class UdpTransport {
 public:
  UdpTransport() noexcept = default;

  // Binds a non-blocking IPv4 UDP socket. port_be == 0 requests an ephemeral
  // port. On failure returns false and the object stays unbound.
  bool bind(uint32_t addr_be, uint16_t port_be);

  Endpoint localEndpoint() const noexcept;  // meaningful only after bind() succeeded

  bool send(const Endpoint& to, std::span<const std::byte> payload);
  bool tryReceive(PacketSlot& slot);

 private:
  int fd_ = -1;
  Endpoint local_;
};
static_assert(Transport<UdpTransport>);

}  // namespace net
