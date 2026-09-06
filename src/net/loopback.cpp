#include "net/loopback.h"

#include <cstring>

namespace net {

LoopbackTransport::LoopbackTransport(Endpoint self) noexcept : self_(self) {}

void LoopbackTransport::connect(LoopbackTransport& peer) noexcept {
  peer_ = &peer;
  peer.peer_ = this;
}

Endpoint LoopbackTransport::self() const noexcept { return self_; }

bool LoopbackTransport::send(const Endpoint& to, std::span<const std::byte> payload) {
  (void)to;
  if (peer_ == nullptr) return false;
  if (payload.size() > kMaxPacket) return false;
  if (peer_->has_packet_) return false;

  std::memcpy(peer_->inbox_slot_.data.data(), payload.data(), payload.size());
  peer_->inbox_slot_.len = static_cast<uint16_t>(payload.size());
  peer_->inbox_slot_.peer = self_;
  peer_->has_packet_ = true;
  return true;
}

bool LoopbackTransport::tryReceive(PacketSlot& slot) {
  if (!has_packet_) return false;
  slot = inbox_slot_;
  has_packet_ = false;
  return true;
}

}  // namespace net
