#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace net {

struct Endpoint {
  uint32_t addr_be = 0;  // IPv4, network byte order
  uint16_t port_be = 0;  // network byte order
  friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

inline constexpr size_t kMaxPacket = 1200;  // stay under the IP fragmentation threshold

struct PacketSlot {
  Endpoint peer;
  uint16_t len = 0;
  std::array<std::byte, kMaxPacket> data{};
};

}  // namespace net
