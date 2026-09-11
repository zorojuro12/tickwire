#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

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

template <typename T>
concept Transport = requires(T t, const Endpoint& ep, std::span<const std::byte> out,
                              PacketSlot& slot) {
  { t.send(ep, out) } -> std::same_as<bool>;
  { t.tryReceive(slot) } -> std::same_as<bool>;
};

}  // namespace net
