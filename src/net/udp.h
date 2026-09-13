#pragma once

#include <cstdint>
#include <span>

#include "net/transport.h"

namespace net {

// Non-blocking IPv4 UDP transport over a raw POSIX socket.
class UdpTransport {
 public:
  UdpTransport() noexcept = default;
  ~UdpTransport();

  UdpTransport(const UdpTransport&) = delete;
  UdpTransport& operator=(const UdpTransport&) = delete;
  UdpTransport(UdpTransport&& other) noexcept;
  UdpTransport& operator=(UdpTransport&& other) noexcept;

  // Binds a non-blocking IPv4 UDP socket. port_be == 0 requests an ephemeral
  // port. On failure returns false and the object stays unbound.
  bool bind(uint32_t addr_be, uint16_t port_be);

  Endpoint localEndpoint() const noexcept;  // meaningful only after bind() succeeded
  int nativeHandle() const noexcept;        // -1 when unbound

  bool send(const Endpoint& to, std::span<const std::byte> payload);
  bool tryReceive(PacketSlot& slot);

  // Fills up to slots.size() slots in one recvmmsg call; returns how many
  // were filled, 0 when nothing is available. Same per-call oversized cap
  // and oversizedSkipped() accounting as tryReceive. tryReceive itself is
  // left unchanged -- both paths stay available so the unbatched arm remains
  // this variant's baseline.
  size_t receiveBatch(std::span<PacketSlot> slots);

  uint64_t oversizedSkipped() const noexcept { return oversized_skipped_; }

 private:
  int fd_ = -1;
  Endpoint local_;
  uint64_t oversized_skipped_ = 0;
};
static_assert(Transport<UdpTransport>);

}  // namespace net
