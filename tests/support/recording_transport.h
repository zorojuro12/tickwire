#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "net/transport.h"

namespace testsupport {

// A test-local transport stub satisfying net::Transport: records every send
// (endpoint, bytes) into a fixed array and lets the test inject inbound
// packets from arbitrary source endpoints -- something a point-to-point
// LoopbackTransport pair cannot do. Shared by server_test.cpp and
// client_test.cpp.
class RecordingTransport {
 public:
  static constexpr size_t kCapacity = 512;

  struct Sent {
    net::Endpoint to;
    std::array<std::byte, net::kMaxPacket> data{};
    uint16_t len = 0;
  };

  bool send(const net::Endpoint& to, std::span<const std::byte> payload) noexcept {
    if (sent_count_ >= kCapacity || payload.size() > net::kMaxPacket) return false;
    Sent& s = sent_[sent_count_++];
    s.to = to;
    s.len = static_cast<uint16_t>(payload.size());
    std::copy(payload.begin(), payload.end(), s.data.begin());
    return true;
  }

  bool tryReceive(net::PacketSlot& slot) noexcept {
    if (inbox_read_ >= inbox_write_) return false;
    slot = inbox_[inbox_read_ % kCapacity];
    ++inbox_read_;
    return true;
  }

  void inject(const net::Endpoint& from, std::span<const std::byte> bytes) noexcept {
    net::PacketSlot slot;
    slot.peer = from;
    slot.len = static_cast<uint16_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), slot.data.begin());
    inbox_[inbox_write_ % kCapacity] = slot;
    ++inbox_write_;
  }

  size_t sentCount() const noexcept { return sent_count_; }
  const Sent& sentAt(size_t i) const noexcept { return sent_[i]; }
  void clearSent() noexcept { sent_count_ = 0; }

 private:
  std::array<Sent, kCapacity> sent_{};
  size_t sent_count_ = 0;
  std::array<net::PacketSlot, kCapacity> inbox_{};
  size_t inbox_write_ = 0;
  size_t inbox_read_ = 0;
};
static_assert(net::Transport<RecordingTransport>);

}  // namespace testsupport
