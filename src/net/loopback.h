#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "net/transport.h"

namespace net {

inline constexpr size_t kLoopbackCapacity = 256;
static_assert((kLoopbackCapacity & (kLoopbackCapacity - 1)) == 0,
              "kLoopbackCapacity must be a power of two");

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
  size_t inboxSize() const noexcept;

 private:
  Endpoint self_;
  LoopbackTransport* peer_ = nullptr;
  std::array<PacketSlot, kLoopbackCapacity> inbox_{};
  uint64_t write_ = 0;
  uint64_t read_ = 0;
};
static_assert(Transport<LoopbackTransport>);

}  // namespace net
