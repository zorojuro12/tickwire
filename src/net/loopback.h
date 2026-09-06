#pragma once

#include <span>

#include "net/transport.h"

namespace net {

inline constexpr size_t kLoopbackCapacity = 256;

// In-memory, point-to-point, deterministic Transport. A multi-peer switch is
// what P2's authoritative server will want; P1 needs exactly two parties to
// test netcode and to give SimulatedTransport an inner. connect() is the
// extension point.
class LoopbackTransport {
 public:
  explicit LoopbackTransport(Endpoint self) noexcept;

  void connect(LoopbackTransport& peer) noexcept;
  Endpoint self() const noexcept;

  bool send(const Endpoint& to, std::span<const std::byte> payload);
  bool tryReceive(PacketSlot& slot);

 private:
  Endpoint self_;
  LoopbackTransport* peer_ = nullptr;
  PacketSlot inbox_slot_{};
  bool has_packet_ = false;
};
static_assert(Transport<LoopbackTransport>);

}  // namespace net
