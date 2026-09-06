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
  if (peer_->write_ - peer_->read_ == kLoopbackCapacity) return false;

  PacketSlot& dest = peer_->inbox_[peer_->write_ & (kLoopbackCapacity - 1)];
  std::memcpy(dest.data.data(), payload.data(), payload.size());
  dest.len = static_cast<uint16_t>(payload.size());
  dest.peer = self_;
  ++peer_->write_;
  return true;
}

bool LoopbackTransport::tryReceive(PacketSlot& slot) {
  if (read_ == write_) return false;
  slot = inbox_[read_ & (kLoopbackCapacity - 1)];
  ++read_;
  return true;
}

size_t LoopbackTransport::inboxSize() const noexcept {
  return static_cast<size_t>(write_ - read_);
}

}  // namespace net
